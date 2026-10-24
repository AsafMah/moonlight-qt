#include "streaming/stats/sessionstatsrecorder.h"
#include "streaming/stats/sessionsummary.h"
#include "streaming/stats/streamstatsmodel.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimeZone>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

bool near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

QString describe(const char* what, double actual, double expected)
{
    return QStringLiteral("%1: got %2, expected %3")
        .arg(QLatin1String(what))
        .arg(actual)
        .arg(expected);
}

// A window with every stage reported
StatsWindow fullWindow()
{
    StatsWindow window;
    window.width = 3840;
    window.height = 2160;
    window.targetFps = 120;
    window.codec = QStringLiteral("HEVC 10-bit HDR");
    window.decoder = QStringLiteral("hevc (d3d11va)");
    window.renderer = QStringLiteral("D3D11VA");
    window.hardwareDecode = true;
    window.totalFrames = 120;
    window.receivedFrames = 119;
    window.decodedFrames = 119;
    window.renderedFrames = 118;
    window.networkDroppedFrames = 1;
    window.pacerDroppedFrames = 1;
    window.framesWithHostProcessingLatency = 100;
    window.totalHostProcessingLatency = 4000; // 4 ms each, in 0.1 ms
    window.minHostProcessingLatency = 30;
    window.maxHostProcessingLatency = 60;
    window.totalReassemblyTimeUs = 119 * 1000;
    window.totalDecodeTimeUs = 119 * 2000; // 2 ms
    window.totalPacerTimeUs = 118 * 1000;  // 1 ms
    window.totalRenderTimeUs = 118 * 3000; // 3 ms
    window.rttMs = 10;
    window.rttVarianceMs = 2;
    window.totalFps = 120;
    window.receivedFps = 119;
    window.decodedFps = 119;
    window.renderedFps = 118;
    window.bitrateMbps = 80;
    window.videoPackets = 5000;
    window.fecPackets = 500;
    return window;
}

// A fake LiGetMicroseconds() the test advances by hand
struct FakeClock
{
    std::atomic<uint64_t> nowUs{ 10000000 };
    SessionStatsRecorder::Clock clock()
    {
        return [this] {
            return nowUs.load();
        };
    }
};

QDateTime fixedWallTime()
{
    return QDateTime(QDate(2026, 10, 24), QTime(21, 5, 3, 250), QTimeZone(3 * 3600));
}

bool testBuilder(QTextStream& err)
{
    bool ok = true;

    SessionSummaryBuilder builder;
    ok &= require(!builder.build(StatsSnapshot()).valid,
                  QStringLiteral("summary without windows is valid"), err);

    StatsSnapshot a;
    a.valid = true;
    a.sequence = 1;
    a.bitrateMbps = 40;
    a.decodedFps = 120;
    a.latency.hasHost = true;
    a.latency.hostMs = 4;
    a.latency.hasNetwork = true;
    a.latency.networkMs = 5;
    a.latency.decodeMs = 2;
    a.latency.queueMs = 1;
    a.latency.renderMs = 3; // total 15
    a.hasRtt = true;
    a.rttMs = 10;
    a.jitterMs = 1;

    StatsSnapshot b = a;
    b.sequence = 2;
    b.bitrateMbps = 60;
    b.latency.hasHost = false;
    b.latency.hostMs = 0;
    b.latency.decodeMs = 4; // total 13
    b.rttMs = 20;
    b.jitterMs = 3;

    // A stalled window: no bitrate, no decoded frames, no RTT
    StatsSnapshot stall;
    stall.valid = true;
    stall.sequence = 3;
    stall.jitterMs = 2;

    // Invalid snapshots are ignored entirely
    builder.addWindow(StatsSnapshot());
    builder.addWindow(a);
    builder.addWindow(b);
    builder.addWindow(stall);
    ok &= require(builder.windows() == 3, describe("windows", builder.windows(), 3), err);

    StatsSnapshot last = stall;
    last.width = 1920;
    last.height = 1080;
    last.targetFps = 60;
    last.codec = QStringLiteral("AV1");
    last.sessionDurationUs = 90500000;
    last.sessionFramesTotal = 1000;
    last.sessionFramesDecoded = 990;
    last.sessionFramesNetworkDropped = 10;
    last.sessionFramesPacerDropped = 99;
    last.sessionFrameTimes.samples = 900;
    last.sessionFrameTimes.averageMs = 16.7;
    last.sessionFrameTimes.onePercentLowFps = 50;
    last.videoPackets = 2000;
    last.fecPackets = 100;

    const SessionSummary summary = builder.build(last);
    ok &= require(summary.valid && summary.windows == 3, QStringLiteral("summary not valid"), err);
    ok &= require(summary.width == 1920 && summary.height == 1080 && summary.targetFps == 60 &&
                      summary.codec == QLatin1String("AV1"),
                  QStringLiteral("stream description not taken from the last snapshot"), err);
    ok &= require(summary.durationUs == 90500000, QStringLiteral("duration is wrong"), err);
    ok &= require(summary.frameTimes.samples == 900 && summary.frameTimes.onePercentLowFps == 50,
                  QStringLiteral("session frame times not carried"), err);
    ok &= require(near(summary.networkLossPercent, 1.0, 1e-9),
                  describe("network loss", summary.networkLossPercent, 1), err);
    ok &= require(near(summary.pacerDropPercent, 10.0, 1e-9),
                  describe("pacer drops", summary.pacerDropPercent, 10), err);
    ok &= require(summary.hasBitrate && near(summary.averageBitrateMbps, 50, 1e-9) &&
                      summary.peakBitrateMbps == 60,
                  describe("average bitrate", summary.averageBitrateMbps, 50), err);
    ok &= require(summary.hasLatency && near(summary.averageLatencyMs, 14, 1e-9) &&
                      near(summary.maxLatencyMs, 15, 1e-9),
                  describe("average latency", summary.averageLatencyMs, 14), err);
    ok &= require(summary.averageLatency.hasHost && near(summary.averageLatency.hostMs, 4, 1e-9),
                  QStringLiteral("host latency must average only windows that report it"), err);
    ok &= require(near(summary.averageLatency.decodeMs, 3, 1e-9) &&
                      near(summary.averageLatency.networkMs, 5, 1e-9),
                  QStringLiteral("per-stage latency averages are wrong"), err);
    ok &= require(summary.hasRtt && near(summary.averageRttMs, 15, 1e-9) && summary.maxRttMs == 20,
                  describe("average RTT", summary.averageRttMs, 15), err);
    ok &= require(near(summary.averageJitterMs, 2, 1e-9),
                  describe("average jitter", summary.averageJitterMs, 2), err);
    ok &= require(summary.fecRecoveredFrames == -1 && summary.fecUnrecoverableFrames == -1,
                  QStringLiteral("unreported FEC results are not -1"), err);

    const QVariantMap map = summary.toVariantMap();
    ok &= require(map.value(QStringLiteral("valid")).toBool() &&
                      near(map.value(QStringLiteral("durationSeconds")).toDouble(), 90.5, 1e-9) &&
                      map.value(QStringLiteral("averageBitrateMbps")).toDouble() == 50 &&
                      map.value(QStringLiteral("onePercentLowFps")).toDouble() == 50,
                  QStringLiteral("variant map values are wrong"), err);
    ok &= require(!map.contains(QStringLiteral("fecRecoveredFrames")) &&
                      !map.contains(QStringLiteral("csvPath")),
                  QStringLiteral("variant map has unknown values"), err);
    ok &= require(summary.toText().contains(QStringLiteral("0:01:30")),
                  QStringLiteral("text summary duration is wrong:\n") + summary.toText(), err);

    // Nothing measurable at all
    SessionSummaryBuilder empty;
    empty.addWindow(stall);
    const SessionSummary bare = empty.build(stall);
    ok &= require(bare.valid && !bare.hasBitrate && !bare.hasLatency && !bare.hasRtt,
                  QStringLiteral("missing values are reported as known"), err);
    const QVariantMap bareMap = bare.toVariantMap();
    ok &= require(!bareMap.contains(QStringLiteral("averageBitrateMbps")) &&
                      !bareMap.contains(QStringLiteral("averageLatencyMs")) &&
                      !bareMap.contains(QStringLiteral("averageRttMs")),
                  QStringLiteral("variant map has values that were never measured"), err);

    return ok;
}

bool testCsvFormat(QTextStream& err)
{
    bool ok = true;

    const QStringList columns = SessionStatsRecorder::csvColumns();
    ok &= require(columns.size() == 43, describe("column count", columns.size(), 43), err);
    ok &= require(columns.first() == QLatin1String("timestamp") &&
                      columns.last() == QLatin1String("frames_pacer_dropped"),
                  QStringLiteral("column order changed"), err);
    ok &= require(SessionStatsRecorder::csvHeader() == columns.join(QLatin1Char(',')),
                  QStringLiteral("header is not the joined columns"), err);

    // A full snapshot from the model
    StreamStatsModel model;
    model.recordFrameArrival(1000000, 0);
    for (int i = 0; i < 200; i++) {
        model.recordFramePresented(1000000 + i * 8333);
    }
    StatsWindow window = fullWindow();
    window.fecRecoveredFrames = 7;
    window.fecUnrecoverableFrames = 1;
    model.publishWindow(window, window, 2700000);
    const StatsSnapshot snapshot = model.snapshot(2700000);

    const QString row = SessionStatsRecorder::csvRow(snapshot, fixedWallTime());
    const QStringList fields = row.split(QLatin1Char(','));
    ok &= require(fields.size() == columns.size(),
                  describe("fields in a full row", fields.size(), columns.size()), err);
    if (fields.size() != columns.size()) {
        err << row << '\n';
        return false;
    }

    auto field = [&](const char* name) {
        return fields.at(columns.indexOf(QLatin1String(name)));
    };
    ok &= require(field("timestamp") == QLatin1String("2026-10-24T21:05:03.250+03:00"),
                  QStringLiteral("timestamp is not ISO 8601 with offset: ") + field("timestamp"),
                  err);
    ok &= require(field("elapsed_s") == QLatin1String("1.700"),
                  QStringLiteral("elapsed_s: ") + field("elapsed_s"), err);
    ok &= require(field("width") == QLatin1String("3840") &&
                      field("codec") == QLatin1String("HEVC 10-bit HDR") &&
                      field("hw_decode") == QLatin1String("1"),
                  QStringLiteral("description fields are wrong"), err);
    ok &= require(field("rtt_ms") == QLatin1String("10") &&
                      field("latency_network_ms") == QLatin1String("5.00") &&
                      field("latency_host_ms") == QLatin1String("4.00") &&
                      field("latency_total_ms") == QLatin1String("15.00"),
                  QStringLiteral("latency fields are wrong: ") + row, err);
    ok &= require(field("bitrate_mbps") == QLatin1String("80.000"),
                  QStringLiteral("bitrate_mbps: ") + field("bitrate_mbps"), err);
    ok &= require(field("fec_recovered_frames") == QLatin1String("7") &&
                      field("fec_unrecoverable_frames") == QLatin1String("1"),
                  QStringLiteral("FEC fields are wrong"), err);
    ok &= require(field("frames_total") == QLatin1String("120") &&
                      field("frames_pacer_dropped") == QLatin1String("1"),
                  QStringLiteral("session totals are wrong"), err);
    ok &= require(!field("low_1pct_fps").isEmpty(), QStringLiteral("1% low is missing"), err);

    // Unknown values are empty fields, never zeros
    StatsSnapshot unknown;
    unknown.valid = true;
    unknown.codec = QStringLiteral("H.264, \"main\"");
    const QString unknownRow = SessionStatsRecorder::csvRow(unknown, fixedWallTime());
    ok &= require(unknownRow.contains(QStringLiteral("\"H.264, \"\"main\"\"\"")),
                  QStringLiteral("text field is not quoted: ") + unknownRow, err);
    QString unquoted = unknownRow;
    unquoted.replace(QStringLiteral("\"H.264, \"\"main\"\"\""), QStringLiteral("codec"));
    const QStringList unknownFields = unquoted.split(QLatin1Char(','));
    ok &= require(unknownFields.size() == columns.size(),
                  describe("fields in an empty row", unknownFields.size(), columns.size()), err);
    if (unknownFields.size() == columns.size()) {
        auto unknownField = [&](const char* name) {
            return unknownFields.at(columns.indexOf(QLatin1String(name)));
        };
        for (const char* name : { "rtt_ms", "bitrate_mbps", "latency_host_ms", "latency_network_ms",
                                  "fec_recovered_frames", "fec_unrecoverable_frames",
                                  "frametime_p99_ms", "host_min_ms" }) {
            ok &= require(unknownField(name).isEmpty(),
                          QStringLiteral("unknown %1 is not empty: %2")
                              .arg(QLatin1String(name), unknownField(name)),
                          err);
        }
    }

    return ok;
}

bool testFileName(QTextStream& err)
{
    bool ok = true;

    const QString name = SessionStatsRecorder::csvFileName(
        QStringLiteral("GAMING-PC"), QStringLiteral("Steam: Big Picture?"), fixedWallTime());
    ok &= require(name == QLatin1String("GAMING-PC_Steam_ Big Picture__20261024-210503.csv"),
                  QStringLiteral("file name: ") + name, err);

    const QString odd = SessionStatsRecorder::csvFileName(QStringLiteral("a/b\\c"),
                                                          QStringLiteral("..."), fixedWallTime());
    ok &= require(odd == QLatin1String("a_b_c_unknown_20261024-210503.csv"),
                  QStringLiteral("file name: ") + odd, err);

    const QString chinese = SessionStatsRecorder::csvFileName(
        QStringLiteral("主机"), QStringLiteral("原神"), fixedWallTime());
    ok &= require(chinese == QStringLiteral("主机_原神_20261024-210503.csv"),
                  QStringLiteral("non-ASCII names must be kept: ") + chinese, err);

    return ok;
}

QStringList readLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

bool testRecorder(QTextStream& err)
{
    bool ok = true;

    QTemporaryDir dir;
    ok &= require(dir.isValid(), QStringLiteral("no temporary directory"), err);

    StreamStatsModel model;
    FakeClock clock;
    SessionStatsRecorder::Options options;
    options.host = QStringLiteral("host");
    options.app = QStringLiteral("app");
    options.writeCsv = true;
    options.directory = dir.filePath(QStringLiteral("stats"));

    SessionStatsRecorder recorder(model, options, clock.clock(), fixedWallTime);
    ok &= require(!recorder.poll(), QStringLiteral("poll recorded a window before any"), err);
    ok &= require(recorder.csvPath().isEmpty(),
                  QStringLiteral("CSV created before the first window"), err);

    model.recordFrameArrival(clock.nowUs, 0);
    StatsWindow window = fullWindow();
    for (int i = 0; i < 3; i++) {
        clock.nowUs += 1000000;
        window.bitrateMbps = 40 + 20 * i; // 40, 60, 80
        model.publishWindow(window, window, clock.nowUs);
        ok &= require(recorder.poll(), QStringLiteral("poll missed window %1").arg(i), err);
        ok &=
            require(!recorder.poll(), QStringLiteral("poll recorded window %1 twice").arg(i), err);
    }

    // Published but not polled: finish() must still pick it up
    clock.nowUs += 1000000;
    window.bitrateMbps = 100;
    model.publishWindow(window, window, clock.nowUs);

    const SessionSummary summary = recorder.finish();
    ok &= require(summary.valid && summary.windows == 4,
                  describe("recorded windows", summary.windows, 4), err);
    ok &= require(near(summary.averageBitrateMbps, 70, 1e-9) && summary.peakBitrateMbps == 100,
                  describe("average bitrate", summary.averageBitrateMbps, 70), err);
    ok &= require(summary.host == QLatin1String("host") && summary.app == QLatin1String("app") &&
                      summary.startTime == fixedWallTime(),
                  QStringLiteral("session identity is wrong"), err);
    ok &= require(summary.framesTotal == 480, describe("frames", summary.framesTotal, 480), err);
    ok &= require(summary.durationUs == 4000000, describe("duration", summary.durationUs, 4000000),
                  err);

    ok &= require(!summary.csvPath.isEmpty() &&
                      summary.csvPath.endsWith(QStringLiteral("host_app_20261024-210503.csv")),
                  QStringLiteral("CSV path: ") + summary.csvPath, err);
    const QStringList lines = readLines(summary.csvPath);
    ok &= require(lines.size() == 5, describe("CSV lines", lines.size(), 5), err);
    if (!lines.isEmpty()) {
        ok &= require(lines.first() == SessionStatsRecorder::csvHeader(),
                      QStringLiteral("CSV does not start with the header"), err);
    }

    // finish() is idempotent and polling stops
    model.publishWindow(window, window, clock.nowUs);
    ok &= require(!recorder.poll(), QStringLiteral("poll recorded after finish"), err);
    ok &= require(recorder.finish().windows == 4, QStringLiteral("second finish changed"), err);

    // A second session in the same second gets its own file
    SessionStatsRecorder second(model, options, clock.clock(), fixedWallTime);
    clock.nowUs += 1000000;
    model.publishWindow(window, window, clock.nowUs);
    const SessionSummary secondSummary = second.finish();
    ok &= require(secondSummary.csvPath.endsWith(QStringLiteral("host_app_20261024-210503-2.csv")),
                  QStringLiteral("second CSV path: ") + secondSummary.csvPath, err);

    // Without CSV recording only the summary is built
    StreamStatsModel quietModel;
    options.writeCsv = false;
    options.directory = dir.filePath(QStringLiteral("quiet"));
    SessionStatsRecorder quiet(quietModel, options, clock.clock(), fixedWallTime);
    quietModel.publishWindow(window, window, clock.nowUs);
    const SessionSummary quietSummary = quiet.finish();
    ok &= require(quietSummary.valid && quietSummary.csvPath.isEmpty() &&
                      !QFile::exists(options.directory),
                  QStringLiteral("CSV written while disabled"), err);

    // No windows at all: an invalid summary and no file
    StreamStatsModel idleModel;
    options.writeCsv = true;
    options.directory = dir.filePath(QStringLiteral("idle"));
    SessionStatsRecorder idle(idleModel, options, clock.clock(), fixedWallTime);
    const SessionSummary idleSummary = idle.finish();
    ok &= require(!idleSummary.valid && !QFile::exists(options.directory),
                  QStringLiteral("idle session produced a summary or a file"), err);

    return ok;
}

bool testRecorderThread(QTextStream& err)
{
    bool ok = true;

    QTemporaryDir dir;
    StreamStatsModel model;
    FakeClock clock;
    SessionStatsRecorder::Options options;
    options.host = QStringLiteral("threaded");
    options.app = QStringLiteral("app");
    options.writeCsv = true;
    options.directory = dir.path();
    options.pollIntervalMs = 10;

    SessionStatsRecorder recorder(model, options, clock.clock(), fixedWallTime);
    recorder.start();

    StatsWindow window = fullWindow();
    for (int i = 0; i < 5; i++) {
        clock.nowUs += 1000000;
        model.publishWindow(window, window, clock.nowUs);
        // Give the worker a few poll intervals to pick it up
        for (int wait = 0; wait < 100 && !QFile::exists(recorder.csvPath()); wait++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    const SessionSummary summary = recorder.finish();
    ok &= require(summary.windows == 5, describe("threaded windows", summary.windows, 5), err);
    ok &= require(readLines(summary.csvPath).size() == 6,
                  describe("threaded CSV lines", readLines(summary.csvPath).size(), 6), err);

    // Destroying a running recorder stops its thread
    {
        SessionStatsRecorder dropped(model, options, clock.clock(), fixedWallTime);
        dropped.start();
    }

    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testBuilder(err);
    ok &= testCsvFormat(err);
    ok &= testFileName(err);
    ok &= testRecorder(err);
    ok &= testRecorderThread(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
