#include "statsoverlaysettings.h"
#include "streaming/video/overlaystatslayout.h"

#include <QSettings>
#include <QSize>
#include <QVariantMap>

#define SER_STATSOVERLAY "statsoverlay"
#define SER_PRESET "preset"
#define SER_SCALE "scale"
#define SER_OPACITY "opacity"
#define SER_METRICS "metrics"
#define SER_DISPLAYS "displays"
#define SER_ANCHOR "anchor"
#define SER_SIZE "size"
#define SER_DETACHED "detached"
#define SER_DETACHED_POS "detached/pos"

using namespace StatsOverlay;

namespace {

QString key(const char* name)
{
    return QStringLiteral(SER_STATSOVERLAY "/") + QLatin1String(name);
}

QString displayGroup(const QString& displayKey)
{
    return QStringLiteral(SER_STATSOVERLAY "/" SER_DISPLAYS "/") + displayKey;
}

Preset visiblePreset(Preset preset)
{
    return preset == Preset::Off ? StatsOverlaySettings::DefaultPreset : preset;
}

} // namespace

StatsOverlaySettings::StatsOverlaySettings(QObject* parent) : QObject(parent) {}

void StatsOverlaySettings::setPreset(int preset)
{
    if (preset <= static_cast<int>(Preset::Off) || preset > static_cast<int>(Preset::Custom)) {
        return;
    }

    const Preset value = static_cast<Preset>(preset);
    if (value == presetValue()) {
        return;
    }

    QSettings().setValue(key(SER_PRESET), presetId(value));
    emit presetChanged();
}

Preset StatsOverlaySettings::presetValue() const
{
    const QString id = QSettings().value(key(SER_PRESET)).toString();
    return visiblePreset(presetFromId(id, DefaultPreset));
}

bool StatsOverlaySettings::detached() const
{
    return QSettings().value(key(SER_DETACHED), false).toBool();
}

void StatsOverlaySettings::setDetached(bool detached)
{
    if (detached == this->detached()) {
        return;
    }

    QSettings().setValue(key(SER_DETACHED), detached);
    emit detachedChanged();
}

int StatsOverlaySettings::scalePercent() const
{
    return clampScalePercent(QSettings().value(key(SER_SCALE), DefaultScalePercent).toInt());
}

void StatsOverlaySettings::setScalePercent(int percent)
{
    percent = clampScalePercent(percent);
    if (percent == scalePercent()) {
        return;
    }

    QSettings().setValue(key(SER_SCALE), percent);
    emit scalePercentChanged();
}

int StatsOverlaySettings::opacityPercent() const
{
    return clampOpacityPercent(QSettings().value(key(SER_OPACITY), DefaultOpacityPercent).toInt());
}

void StatsOverlaySettings::setOpacityPercent(int percent)
{
    percent = clampOpacityPercent(percent);
    if (percent == opacityPercent()) {
        return;
    }

    QSettings().setValue(key(SER_OPACITY), percent);
    emit opacityPercentChanged();
}

QVector<Metric> StatsOverlaySettings::metricList() const
{
    QSettings settings;
    if (!settings.contains(key(SER_METRICS))) {
        return defaultCustomMetrics();
    }
    return parseMetricList(settings.value(key(SER_METRICS)).toString());
}

QStringList StatsOverlaySettings::metrics() const
{
    QStringList ids;
    for (Metric metric : metricList()) {
        ids.append(metricId(metric));
    }
    return ids;
}

QVariantList StatsOverlaySettings::availableMetrics() const
{
    QVariantList list;
    for (Metric metric : allMetrics()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), metricId(metric));
        entry.insert(QStringLiteral("name"), metricName(metric));
        list.append(entry);
    }
    return list;
}

bool StatsOverlaySettings::isMetricEnabled(const QString& id) const
{
    const std::optional<Metric> metric = metricFromId(id);
    return metric && metricList().contains(*metric);
}

void StatsOverlaySettings::setMetricEnabled(const QString& id, bool enabled)
{
    const std::optional<Metric> metric = metricFromId(id);
    if (!metric) {
        return;
    }

    QVector<Metric> list = metricList();
    if (list.contains(*metric) == enabled) {
        return;
    }

    if (enabled) {
        list.append(*metric);
    } else {
        list.removeAll(*metric);
    }

    // An empty string is a valid, explicit "nothing selected"
    QSettings().setValue(key(SER_METRICS), formatMetricList(list));
    emit metricsChanged();
}

void StatsOverlaySettings::resetPlacements()
{
    QSettings().remove(QStringLiteral(SER_STATSOVERLAY "/" SER_DISPLAYS));
    emit sizesChanged();
}

void StatsOverlaySettings::resetSizes()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral(SER_STATSOVERLAY "/" SER_DISPLAYS));
    for (const QString& display : settings.childGroups()) {
        settings.remove(display + QStringLiteral("/" SER_SIZE));
    }
    settings.endGroup();
    emit sizesChanged();
}

QString StatsOverlaySettings::sizeSummary() const
{
    QStringList entries;

    QSettings settings;
    settings.beginGroup(QStringLiteral(SER_STATSOVERLAY "/" SER_DISPLAYS));
    const QStringList displays = settings.childGroups();
    settings.endGroup();

    for (const QString& display : displays) {
        for (Preset preset : { Preset::Detailed, Preset::Custom }) {
            const QSize size = displaySize(display, preset);
            if (!size.isValid()) {
                continue;
            }
            // displayKey() turns the backslashes and dots of Windows display
            // names into underscores
            QString name = display;
            while (name.startsWith(QLatin1Char('_'))) {
                name.remove(0, 1);
            }
            entries.append(tr("%1 on %2: %3 × %4")
                               .arg(preset == Preset::Detailed ? tr("Detailed") : tr("Custom"),
                                    name.isEmpty() ? display : name)
                               .arg(size.width())
                               .arg(size.height()));
        }
    }

    return entries.isEmpty() ? tr("Default") : entries.join(QLatin1Char('\n'));
}

void StatsOverlaySettings::reload()
{
    emit presetChanged();
    emit scalePercentChanged();
    emit opacityPercentChanged();
    emit metricsChanged();
    emit detachedChanged();
}

StatsOverlaySettings::DisplayState
StatsOverlaySettings::displayState(const QString& displayKey) const
{
    DisplayState state;

    QSettings settings;
    settings.beginGroup(displayGroup(displayKey));
    if (settings.contains(QStringLiteral(SER_ANCHOR))) {
        state.hasAnchor = true;
        state.anchor =
            anchorFromId(settings.value(QStringLiteral(SER_ANCHOR)).toString(), Anchor::TopLeft);
    }
    if (settings.contains(QStringLiteral(SER_PRESET))) {
        const Preset preset =
            presetFromId(settings.value(QStringLiteral(SER_PRESET)).toString(), Preset::Off);
        if (preset != Preset::Off) {
            state.hasPreset = true;
            state.preset = preset;
        }
    }
    settings.endGroup();

    return state;
}

void StatsOverlaySettings::setDisplayAnchor(const QString& displayKey, Anchor anchor)
{
    QSettings().setValue(displayGroup(displayKey) + QStringLiteral("/" SER_ANCHOR),
                         anchorId(anchor));
}

void StatsOverlaySettings::setDisplayPreset(const QString& displayKey, Preset preset)
{
    if (preset == Preset::Off) {
        return;
    }
    QSettings().setValue(displayGroup(displayKey) + QStringLiteral("/" SER_PRESET),
                         presetId(preset));
}

QSize StatsOverlaySettings::displaySize(const QString& displayKey, Preset preset) const
{
    if (!presetIsResizable(preset)) {
        return QSize();
    }
    const QSize size =
        QSettings()
            .value(displayGroup(displayKey) + QStringLiteral("/" SER_SIZE "/") + presetId(preset))
            .toSize();
    return size.isValid() && !size.isEmpty() ? size : QSize();
}

void StatsOverlaySettings::setDisplaySize(const QString& displayKey, Preset preset,
                                          const QSize& size)
{
    if (!presetIsResizable(preset)) {
        return;
    }
    const QString key =
        displayGroup(displayKey) + QStringLiteral("/" SER_SIZE "/") + presetId(preset);
    if (size.isValid()) {
        QSettings().setValue(key, size);
    } else {
        QSettings().remove(key);
    }
    emit sizesChanged();
}

std::optional<QPoint> StatsOverlaySettings::detachedPosition(const QString& displayKey) const
{
    const QVariant value =
        QSettings().value(displayGroup(displayKey) + QStringLiteral("/" SER_DETACHED_POS));
    if (!value.isValid()) {
        return std::nullopt;
    }
    return value.toPoint();
}

void StatsOverlaySettings::setDetachedPosition(const QString& displayKey, const QPoint& position)
{
    QSettings().setValue(displayGroup(displayKey) + QStringLiteral("/" SER_DETACHED_POS), position);
}

QString StatsOverlaySettings::metricName(Metric metric)
{
    switch (metric) {
    case Metric::Resolution:
        return tr("Resolution and codec");
    case Metric::Fps:
        return tr("Frame rate");
    case Metric::OnePercentLow:
        return tr("1% low frame rate");
    case Metric::Latency:
        return tr("Estimated latency");
    case Metric::Bitrate:
        return tr("Bitrate");
    case Metric::Loss:
        return tr("Frames lost by the network");
    case Metric::Rtt:
        return tr("Round trip time");
    case Metric::Jitter:
        return tr("Network jitter");
    case Metric::Host:
        return tr("Host processing time");
    case Metric::Decode:
        return tr("Decode time");
    case Metric::Queue:
        return tr("Frame queue delay");
    case Metric::Render:
        return tr("Render time");
    case Metric::Fec:
        return tr("FEC");
    case Metric::Abr:
        return tr("Adaptive bitrate");
    case Metric::PacerDrops:
        return tr("Frames dropped by pacing");
    case Metric::DisplayHz:
        return tr("Display refresh rate");
    case Metric::SessionTime:
        return tr("Session time");
    case Metric::FrameTimeGraph:
        return tr("Frame time graph");
    case Metric::LatencyBar:
        return tr("Latency breakdown bar");
    case Metric::Present:
        return tr("Pre-Present / Present");
    }
    return QString();
}

QString StatsOverlaySettings::metricDescription(Metric metric)
{
    switch (metric) {
    case Metric::Resolution:
        return tr("The resolution, frame rate and codec the host is streaming.");
    case Metric::Fps:
        return tr("Frames shown per second. It should match the stream's frame rate; lower means "
                  "frames are lost or arrive late.");
    case Metric::OnePercentLow:
        return tr("The frame rate of the slowest 1% of frames. Close to the FPS is smooth; much "
                  "lower means stutter.");
    case Metric::Latency:
        return tr("Estimated time from the host capturing a frame to it appearing here. Under "
                  "40 ms is good; over 80 ms feels sluggish.");
    case Metric::Bitrate:
        return tr("Video data received per second, and the bitrate the host was asked for. "
                  "Higher gives a sharper picture, as long as the network keeps up.");
    case Metric::Loss:
        return tr("Frames lost on the network. Above 0.25% shows as glitches; above 1% the "
                  "network is struggling.");
    case Metric::Rtt:
        return tr("Round trip time to the host, and how much it varies. Under 20 ms is good; "
                  "over 50 ms adds noticeable input delay.");
    case Metric::Jitter:
        return tr("How much the arrival time of video packets varies. Under 2 ms is good; more "
                  "makes frame pacing uneven.");
    case Metric::Host:
        return tr("Time the host spent capturing and encoding each frame. Lower is better.");
    case Metric::Decode:
        return tr("Time this computer takes to decode a frame. Under 5 ms is good; over 8 ms "
                  "can limit the frame rate.");
    case Metric::Queue:
        return tr("Time a decoded frame waits before it is shown. It should stay low; high "
                  "values add latency.");
    case Metric::Render:
        return tr("Time to draw a decoded frame on screen. Lower is better.");
    case Metric::Fec:
        return tr("Share of packets that are error correction (parity), and the frames it "
                  "recovered or could not recover. Lost frames mean the network drops more "
                  "than FEC can repair.");
    case Metric::Abr:
        return tr("Whether the host adapts the bitrate to the network automatically.");
    case Metric::PacerDrops:
        return tr("Frames dropped to stay in step with the display. Above 1% shows as stutter.");
    case Metric::DisplayHz:
        return tr("Refresh rate of this display, and the stream's frame rate. A display slower "
                  "than the stream has to drop frames.");
    case Metric::SessionTime:
        return tr("How long this stream has been running.");
    case Metric::FrameTimeGraph:
        return tr("Time between recent frames. Bars above the dashed target line are late "
                  "frames; tall spikes are visible hitches.");
    case Metric::LatencyBar:
        return tr("How the estimated latency splits between the host, the network, decoding, "
                  "queueing and rendering.");
    case Metric::Present:
        return tr("Elapsed time from render start to the Present call, and time spent inside "
                  "Present. The pre-Present phase can include lock waits and GPU or display "
                  "back-pressure.");
    }
    return QString();
}
