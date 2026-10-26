#include "sessionstatsrecorder.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QtDebug>

#include <chrono>
#include <cmath>

#define SER_SESSIONSTATS "sessionstats"
#define SER_RECORDCSV "recordcsv"

namespace {

QString csvField(const QString& value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) ||
        value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        QString quoted = value;
        quoted.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QLatin1Char('"') + quoted + QLatin1Char('"');
    }
    return value;
}

QString number(double value, int decimals)
{
    if (std::isnan(value) || std::isinf(value)) {
        return QString();
    }
    return QString::number(value, 'f', decimals);
}

QString fileNamePart(const QString& value)
{
    QString part;
    part.reserve(value.size());
    for (const QChar c : value) {
        // Reserved on Windows, plus path separators and control characters
        if (c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c)) {
            part += QLatin1Char('_');
        } else {
            part += c;
        }
    }

    part = part.trimmed();
    // Windows drops trailing dots and spaces
    while (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) {
        part.chop(1);
    }
    part = part.left(64);
    if (part.isEmpty()) {
        part = QStringLiteral("unknown");
    }
    return part;
}

QString settingsKey()
{
    return QStringLiteral(SER_SESSIONSTATS "/" SER_RECORDCSV);
}

} // namespace

SessionStatsRecorder::SessionStatsRecorder(const StreamStatsModel& model, Options options,
                                           Clock clock, WallClock wallClock)
    : m_Model(model), m_Options(std::move(options)), m_Clock(std::move(clock)),
      m_WallClock(wallClock ? std::move(wallClock) : [] { return QDateTime::currentDateTime(); }),
      m_StartTime(m_WallClock())
{
}

SessionStatsRecorder::~SessionStatsRecorder()
{
    finish();
}

void SessionStatsRecorder::start()
{
    if (m_Thread.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_StopLock);
        m_StopRequested = false;
    }
    m_Thread = std::thread([this] { run(); });
}

void SessionStatsRecorder::run()
{
    const auto interval = std::chrono::milliseconds(qMax(10, m_Options.pollIntervalMs));

    std::unique_lock<std::mutex> lock(m_StopLock);
    while (!m_StopRequested) {
        lock.unlock();
        poll();
        lock.lock();

        m_StopCondition.wait_for(lock, interval, [this] { return m_StopRequested; });
    }
}

bool SessionStatsRecorder::poll()
{
    std::lock_guard<std::mutex> lock(m_Lock);

    if (m_Finished || m_Model.sequence() == m_LastSequence) {
        return false;
    }

    const uint64_t nowUs = m_Clock();
    const StatsSnapshot snapshot = m_Model.snapshot(nowUs);
    if (!snapshot.valid || snapshot.sequence == m_LastSequence) {
        return false;
    }

    m_LastSequence = snapshot.sequence;
    m_Builder.addWindow(snapshot);

    if (m_Options.writeCsv) {
        writeRow(snapshot, nowUs);
    }
    return true;
}

SessionSummary SessionStatsRecorder::finish()
{
    if (m_Thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_StopLock);
            m_StopRequested = true;
        }
        m_StopCondition.notify_all();
        m_Thread.join();
    }

    // Catch a window published since the last poll
    poll();

    std::lock_guard<std::mutex> lock(m_Lock);

    if (!m_Finished) {
        m_Finished = true;

        m_Summary = m_Builder.build(m_Model.snapshot(m_Clock()));
        m_Summary.host = m_Options.host;
        m_Summary.app = m_Options.app;
        m_Summary.startTime = m_StartTime;
        m_Summary.endTime = m_WallClock();
        m_Summary.csvPath = m_CsvPath;

        if (m_Csv) {
            m_Csv->close();
            m_Csv.reset();
        }
    }

    return m_Summary;
}

QString SessionStatsRecorder::csvPath() const
{
    std::lock_guard<std::mutex> lock(m_Lock);
    return m_CsvPath;
}

bool SessionStatsRecorder::openCsv()
{
    if (m_Csv) {
        return true;
    }
    if (m_CsvFailed) {
        return false;
    }

    QDir dir(m_Options.directory.isEmpty() ? defaultDirectory() : m_Options.directory);
    if (!dir.mkpath(QStringLiteral("."))) {
        qWarning() << "Unable to create session stats directory" << dir.absolutePath();
        m_CsvFailed = true;
        return false;
    }

    const QString fileName = csvFileName(m_Options.host, m_Options.app, m_StartTime);
    QString path = dir.absoluteFilePath(fileName);
    // Never overwrite an earlier session that started in the same second
    for (int i = 2; QFile::exists(path); i++) {
        path = dir.absoluteFilePath(fileName.chopped(4) + QStringLiteral("-%1.csv").arg(i));
    }

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Unable to write session stats to" << path << ":" << file->errorString();
        m_CsvFailed = true;
        return false;
    }

    file->write(csvHeader().toUtf8());
    file->write("\n");
    m_Csv = std::move(file);
    m_CsvPath = QDir::toNativeSeparators(path);
    qInfo() << "Recording session stats to" << m_CsvPath;
    return true;
}

void SessionStatsRecorder::writeRow(const StatsSnapshot& snapshot, uint64_t nowUs)
{
    if (!openCsv()) {
        return;
    }

    // The window was published a little before this poll
    const qint64 ageMs = nowUs > snapshot.timestampUs
                             ? static_cast<qint64>((nowUs - snapshot.timestampUs) / 1000)
                             : 0;
    const QDateTime windowTime = m_WallClock().addMSecs(-ageMs);

    m_Csv->write(csvRow(snapshot, windowTime).toUtf8());
    m_Csv->write("\n");
    // Keep what was recorded if the app goes away mid-stream
    m_Csv->flush();
}

QStringList SessionStatsRecorder::csvColumns()
{
    return {
        QStringLiteral("timestamp"),
        QStringLiteral("elapsed_s"),
        QStringLiteral("width"),
        QStringLiteral("height"),
        QStringLiteral("target_fps"),
        QStringLiteral("codec"),
        QStringLiteral("decoder"),
        QStringLiteral("hw_decode"),
        QStringLiteral("total_fps"),
        QStringLiteral("received_fps"),
        QStringLiteral("decoded_fps"),
        QStringLiteral("rendered_fps"),
        QStringLiteral("frametime_avg_ms"),
        QStringLiteral("frametime_max_ms"),
        QStringLiteral("frametime_p99_ms"),
        QStringLiteral("frametime_p999_ms"),
        QStringLiteral("low_1pct_fps"),
        QStringLiteral("low_01pct_fps"),
        QStringLiteral("rtt_ms"),
        QStringLiteral("rtt_var_ms"),
        QStringLiteral("jitter_ms"),
        QStringLiteral("network_loss_pct"),
        QStringLiteral("pacer_drop_pct"),
        QStringLiteral("bitrate_mbps"),
        QStringLiteral("latency_host_ms"),
        QStringLiteral("latency_network_ms"),
        QStringLiteral("latency_decode_ms"),
        QStringLiteral("latency_queue_ms"),
        QStringLiteral("latency_render_ms"),
        QStringLiteral("latency_total_ms"),
        QStringLiteral("host_min_ms"),
        QStringLiteral("host_max_ms"),
        QStringLiteral("reassembly_ms"),
        QStringLiteral("video_packets"),
        QStringLiteral("fec_packets"),
        QStringLiteral("fec_recovered_frames"),
        QStringLiteral("fec_unrecoverable_frames"),
        QStringLiteral("frames_total"),
        QStringLiteral("frames_received"),
        QStringLiteral("frames_decoded"),
        QStringLiteral("frames_rendered"),
        QStringLiteral("frames_network_dropped"),
        QStringLiteral("frames_pacer_dropped"),
        QStringLiteral("render_submit_ms"),
        QStringLiteral("render_present_ms"),
    };
}

QString SessionStatsRecorder::csvHeader()
{
    return csvColumns().join(QLatin1Char(','));
}

QString SessionStatsRecorder::csvRow(const StatsSnapshot& s, const QDateTime& windowTime)
{
    const LatencyBreakdown& latency = s.latency;
    const FrameTimeSummary& frames = s.frameTimes;
    const bool hasFrames = frames.samples > 0;

    QStringList fields;
    fields.reserve(45);
    fields << windowTime.toOffsetFromUtc(windowTime.offsetFromUtc()).toString(Qt::ISODateWithMs);
    fields << number(static_cast<double>(s.sessionDurationUs) / 1000000.0, 3);
    fields << QString::number(s.width) << QString::number(s.height) << QString::number(s.targetFps);
    fields << csvField(s.codec) << csvField(s.decoder);
    fields << QString::number(s.hardwareDecode ? 1 : 0);
    fields << number(s.totalFps, 2) << number(s.receivedFps, 2) << number(s.decodedFps, 2)
           << number(s.renderedFps, 2);
    if (hasFrames) {
        fields << number(frames.averageMs, 3) << number(frames.maxMs, 3) << number(frames.p99Ms, 3)
               << number(frames.p999Ms, 3) << number(frames.onePercentLowFps, 2)
               << number(frames.pointOnePercentLowFps, 2);
    } else {
        fields << QString() << QString() << QString() << QString() << QString() << QString();
    }
    if (s.hasRtt) {
        fields << QString::number(s.rttMs) << QString::number(s.rttVarianceMs);
    } else {
        fields << QString() << QString();
    }
    fields << number(s.jitterMs, 3);
    fields << number(s.networkLossPercent, 3) << number(s.pacerDropPercent, 3);
    fields << number(s.bitrateMbps, 3);
    fields << (latency.hasHost ? number(latency.hostMs, 2) : QString());
    fields << (latency.hasNetwork ? number(latency.networkMs, 2) : QString());
    fields << number(latency.decodeMs, 2)
           << (latency.hasQueue ? number(latency.queueMs, 2) : QString())
           << number(latency.renderMs, 2) << number(latency.totalMs(), 2);
    if (latency.hasHost) {
        fields << number(s.hostMinMs, 1) << number(s.hostMaxMs, 1);
    } else {
        fields << QString() << QString();
    }
    fields << number(s.reassemblyMs, 3);
    fields << QString::number(s.videoPackets) << QString::number(s.fecPackets);
    fields << (s.fecRecoveredFrames >= 0 ? QString::number(s.fecRecoveredFrames) : QString());
    fields << (s.fecUnrecoverableFrames >= 0 ? QString::number(s.fecUnrecoverableFrames)
                                             : QString());
    fields << QString::number(s.sessionFramesTotal) << QString::number(s.sessionFramesReceived)
           << QString::number(s.sessionFramesDecoded) << QString::number(s.sessionFramesRendered)
           << QString::number(s.sessionFramesNetworkDropped)
           << QString::number(s.sessionFramesPacerDropped);
    if (s.presentTiming.measured) {
        fields << number(s.presentTiming.prePresentMs, 3) << number(s.presentTiming.presentMs, 3);
    } else {
        fields << QString() << QString();
    }

    return fields.join(QLatin1Char(','));
}

QString SessionStatsRecorder::csvFileName(const QString& host, const QString& app,
                                          const QDateTime& start)
{
    return QStringLiteral("%1_%2_%3.csv")
        .arg(fileNamePart(host), fileNamePart(app),
             start.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QString SessionStatsRecorder::defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("stats"));
}

bool SessionStatsRecorder::csvRecordingEnabled()
{
    return QSettings().value(settingsKey(), false).toBool();
}

void SessionStatsRecorder::setCsvRecordingEnabled(bool enabled)
{
    QSettings().setValue(settingsKey(), enabled);
}
