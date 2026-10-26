#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <optional>

// Presets, metrics, thresholds and placement math of the performance overlay
// window (OverlayStatsWindow). QtCore only, so it is unit tested on its own
// (tests/stats_overlay_layout).
namespace StatsOverlay {

enum class Preset
{
    Off = 0,
    Minimal,  // one chip: FPS, 1% low, latency
    Compact,  // one bar of cells
    Detailed, // panel with frame time graph, latency bar and a key/value grid
    Custom,   // the metrics the user picked
};

// Stable ids, persisted in settings: "off", "minimal", ...
QString presetId(Preset preset);
Preset presetFromId(const QString& id, Preset fallback);

// Off -> Minimal -> Compact -> Detailed -> Custom -> Off
Preset nextPreset(Preset preset);

// Metrics of the Custom preset, in display order
enum class Metric
{
    Resolution, // resolution, frame rate and codec
    Fps,
    OnePercentLow,
    Latency,
    Bitrate,
    Loss,
    Rtt,
    Jitter,
    Host,
    Decode,
    Queue,
    Render,
    Fec,
    Abr,
    PacerDrops,
    DisplayHz,
    SessionTime,
    FrameTimeGraph,
    LatencyBar,
    Present,
};

constexpr int MetricCount = static_cast<int>(Metric::Present) + 1;

QVector<Metric> allMetrics();
QString metricId(Metric metric);
std::optional<Metric> metricFromId(const QString& id);

// Comma separated ids. Unknown ids and duplicates are dropped, and the result
// is always in display order.
QVector<Metric> parseMetricList(const QString& list);
QString formatMetricList(const QVector<Metric>& metrics);
QVector<Metric> defaultCustomMetrics();

// Good/warn/bad coloring of a value
enum class Grade
{
    Neutral,
    Good,
    Warn,
    Bad,
};

// Default thresholds. A value is Warn past the first number and Bad past the
// second.
struct Thresholds
{
    double decodeWarnMs = 5;
    double decodeBadMs = 8;
    double lossWarnPercent = 0.25;
    double lossBadPercent = 1;
    double latencyWarnMs = 40;
    double latencyBadMs = 80;
    double rttWarnMs = 20;
    double rttBadMs = 50;
    double jitterWarnMs = 2;
    double jitterBadMs = 5;
    double pacerDropWarnPercent = 1;
    double pacerDropBadPercent = 5;
    // Fraction of the frame rate below which the 1% low is Warn / Bad
    double lowWarnRatio = 0.85;
    double lowBadRatio = 0.70;
    // Fraction of the target frame rate below which FPS is Warn / Bad
    double fpsWarnRatio = 0.95;
    double fpsBadRatio = 0.80;
    // Multiple of the target frame time above which a frame is a spike
    double spikeWarnRatio = 1.5;
    double spikeBadRatio = 2.0;
    double presentWarnRatio = 0.5;
    double presentBadRatio = 1.0;
};

// Higher is worse
Grade gradeAbove(double value, double warn, double bad);
Grade gradeFps(double fps, int targetFps, const Thresholds& thresholds = Thresholds());
Grade gradeLow(double lowFps, double fps, const Thresholds& thresholds = Thresholds());
Grade gradeFrameTime(double frameTimeMs, int targetFps,
                     const Thresholds& thresholds = Thresholds());
Grade gradePresent(double prePresentMs, double presentMs, int targetFps,
                   const Thresholds& thresholds = Thresholds());

// Scale and background opacity limits, in percent
constexpr int MinScalePercent = 75;
constexpr int MaxScalePercent = 200;
constexpr int DefaultScalePercent = 100;
constexpr int MinOpacityPercent = 30;
constexpr int MaxOpacityPercent = 100;
constexpr int DefaultOpacityPercent = 85;

int clampScalePercent(int percent);
int clampOpacityPercent(int percent);

// The nine places the overlay snaps to, row by row
enum class Anchor
{
    TopLeft = 0,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};

QString anchorId(Anchor anchor);
Anchor anchorFromId(const QString& id, Anchor fallback);

// Top-left corner of a window of the given size placed at an anchor of parent,
// keeping margin from the edges it is anchored to
QPoint anchoredPosition(Anchor anchor, const QRect& parent, const QSize& size, int margin);

// The anchor whose third of parent contains point (clamped into parent)
Anchor nearestAnchor(const QRect& parent, const QPoint& point);

// How much of a remembered detached window, in pixels each way, must still be
// on a screen for it to go back there
constexpr int DetachedMinVisible = 48;

// Top-left corner of a detached overlay window of the given size. screens are
// the available geometries of all screens, streamScreen the one the stream is
// on. A remembered position is kept, moved fully onto the screen it overlaps
// most, as long as DetachedMinVisible of it is on that screen. Otherwise the
// window goes margin from the top-left of the first screen apart from the
// stream's, or of the stream's screen when there is no other.
QPoint detachedWindowPosition(const std::optional<QPoint>& saved, const QSize& size,
                              const QVector<QRect>& screens, const QRect& streamScreen, int margin);

// A display name made safe for use as one QSettings key segment
QString displayKey(const QString& screenName);

// "1:02:03" or "2:03"
QString formatDuration(qint64 seconds);

} // namespace StatsOverlay
