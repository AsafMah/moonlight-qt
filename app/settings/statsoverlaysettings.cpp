#include "statsoverlaysettings.h"

#include <QSettings>
#include <QVariantMap>

#define SER_STATSOVERLAY "statsoverlay"
#define SER_PRESET "preset"
#define SER_SCALE "scale"
#define SER_OPACITY "opacity"
#define SER_METRICS "metrics"
#define SER_DISPLAYS "displays"
#define SER_ANCHOR "anchor"

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
}

void StatsOverlaySettings::reload()
{
    emit presetChanged();
    emit scalePercentChanged();
    emit opacityPercentChanged();
    emit metricsChanged();
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
    }
    return QString();
}
