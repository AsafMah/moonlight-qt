#pragma once

#include "streaming/video/overlaystatspreset.h"

#include <QObject>
#include <QStringList>
#include <QVariantList>

// Persistent settings of the performance overlay window, shared by the
// settings page (as the StatsOverlaySettings QML singleton) and the overlay
// controller during a stream.
//
// Values are read from QSettings on every access rather than cached, because
// the stream changes some of them (scale from the edit mode wheel, per-display
// placement) while the settings page's instance is alive.
//
// Whether the overlay is shown when a stream starts stays in
// StreamingPreferences::showPerformanceOverlay; "preset" here is the look it
// has when shown, and is never Off.
class StatsOverlaySettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int preset READ preset WRITE setPreset NOTIFY presetChanged)
    Q_PROPERTY(int scalePercent READ scalePercent WRITE setScalePercent NOTIFY scalePercentChanged)
    Q_PROPERTY(
        int opacityPercent READ opacityPercent WRITE setOpacityPercent NOTIFY opacityPercentChanged)
    Q_PROPERTY(QStringList metrics READ metrics NOTIFY metricsChanged)
    Q_PROPERTY(QVariantList availableMetrics READ availableMetrics CONSTANT)
    Q_PROPERTY(int minScalePercent READ minScalePercent CONSTANT)
    Q_PROPERTY(int maxScalePercent READ maxScalePercent CONSTANT)
    Q_PROPERTY(int minOpacityPercent READ minOpacityPercent CONSTANT)
    Q_PROPERTY(int maxOpacityPercent READ maxOpacityPercent CONSTANT)

public:
    // Mirrors StatsOverlay::Preset for QML
    enum PresetValue
    {
        PresetOff = static_cast<int>(StatsOverlay::Preset::Off),
        PresetMinimal = static_cast<int>(StatsOverlay::Preset::Minimal),
        PresetCompact = static_cast<int>(StatsOverlay::Preset::Compact),
        PresetDetailed = static_cast<int>(StatsOverlay::Preset::Detailed),
        PresetCustom = static_cast<int>(StatsOverlay::Preset::Custom),
    };
    Q_ENUM(PresetValue)

    struct DisplayState
    {
        bool hasAnchor = false;
        StatsOverlay::Anchor anchor = StatsOverlay::Anchor::TopLeft;
        bool hasPreset = false;
        StatsOverlay::Preset preset = StatsOverlay::Preset::Compact;
    };

    static constexpr StatsOverlay::Preset DefaultPreset = StatsOverlay::Preset::Compact;

    explicit StatsOverlaySettings(QObject* parent = nullptr);

    int preset() const { return static_cast<int>(presetValue()); }
    void setPreset(int preset);
    StatsOverlay::Preset presetValue() const;

    int scalePercent() const;
    void setScalePercent(int percent);

    int opacityPercent() const;
    void setOpacityPercent(int percent);

    QStringList metrics() const;
    QVector<StatsOverlay::Metric> metricList() const;
    QVariantList availableMetrics() const;

    int minScalePercent() const { return StatsOverlay::MinScalePercent; }
    int maxScalePercent() const { return StatsOverlay::MaxScalePercent; }
    int minOpacityPercent() const { return StatsOverlay::MinOpacityPercent; }
    int maxOpacityPercent() const { return StatsOverlay::MaxOpacityPercent; }

    Q_INVOKABLE bool isMetricEnabled(const QString& id) const;
    Q_INVOKABLE void setMetricEnabled(const QString& id, bool enabled);

    // Forgets every remembered per-display position and preset
    Q_INVOKABLE void resetPlacements();

    // Re-reads everything, for a page that may have missed in-stream changes
    Q_INVOKABLE void reload();

    // Per-display placement and preset, keyed by StatsOverlay::displayKey()
    DisplayState displayState(const QString& displayKey) const;
    void setDisplayAnchor(const QString& displayKey, StatsOverlay::Anchor anchor);
    void setDisplayPreset(const QString& displayKey, StatsOverlay::Preset preset);

    // Translated name of a metric for the settings page
    static QString metricName(StatsOverlay::Metric metric);

signals:
    void presetChanged();
    void scalePercentChanged();
    void opacityPercentChanged();
    void metricsChanged();
};
