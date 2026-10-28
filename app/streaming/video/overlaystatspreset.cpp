#include "overlaystatspreset.h"

#include <QStringList>

#include <algorithm>

namespace StatsOverlay {

namespace {

struct PresetName
{
    Preset preset;
    const char* id;
};

const PresetName k_Presets[] = {
    { Preset::Off, "off" },         { Preset::Minimal, "minimal" },
    { Preset::Compact, "compact" }, { Preset::Detailed, "detailed" },
    { Preset::Custom, "custom" },
};

const char* const k_MetricIds[MetricCount] = {
    "resolution", "fps",  "low1",   "latency", "bitrate",    "loss",    "rtt",
    "jitter",     "host", "decode", "queue",   "render",     "fec",     "abr",
    "pacerdrops", "hz",   "time",   "graph",   "latencybar", "present",
};

const char* const k_AnchorIds[] = {
    "top-left", "top",         "top-right", "left",         "center",
    "right",    "bottom-left", "bottom",    "bottom-right",
};

} // namespace

QString presetId(Preset preset)
{
    for (const PresetName& name : k_Presets) {
        if (name.preset == preset) {
            return QLatin1String(name.id);
        }
    }
    return QStringLiteral("off");
}

Preset presetFromId(const QString& id, Preset fallback)
{
    for (const PresetName& name : k_Presets) {
        if (id.compare(QLatin1String(name.id), Qt::CaseInsensitive) == 0) {
            return name.preset;
        }
    }
    return fallback;
}

Preset nextPreset(Preset preset)
{
    switch (preset) {
    case Preset::Off:
        return Preset::Minimal;
    case Preset::Minimal:
        return Preset::Compact;
    case Preset::Compact:
        return Preset::Detailed;
    case Preset::Detailed:
        return Preset::Custom;
    case Preset::Custom:
    default:
        return Preset::Off;
    }
}

QVector<Metric> allMetrics()
{
    QVector<Metric> metrics;
    for (int i = 0; i < MetricCount; i++) {
        metrics.append(static_cast<Metric>(i));
    }
    return metrics;
}

QString metricId(Metric metric)
{
    const int index = static_cast<int>(metric);
    if (index < 0 || index >= MetricCount) {
        return QString();
    }
    return QLatin1String(k_MetricIds[index]);
}

std::optional<Metric> metricFromId(const QString& id)
{
    const QString trimmed = id.trimmed();
    for (int i = 0; i < MetricCount; i++) {
        if (trimmed.compare(QLatin1String(k_MetricIds[i]), Qt::CaseInsensitive) == 0) {
            return static_cast<Metric>(i);
        }
    }
    return std::nullopt;
}

QVector<Metric> parseMetricList(const QString& list)
{
    bool present[MetricCount] = {};
    for (const QString& id : list.split(QLatin1Char(','))) {
        if (const std::optional<Metric> metric = metricFromId(id)) {
            present[static_cast<int>(*metric)] = true;
        }
    }

    QVector<Metric> metrics;
    for (int i = 0; i < MetricCount; i++) {
        if (present[i]) {
            metrics.append(static_cast<Metric>(i));
        }
    }
    return metrics;
}

QString formatMetricList(const QVector<Metric>& metrics)
{
    QStringList ids;
    for (Metric metric : allMetrics()) {
        if (metrics.contains(metric)) {
            ids.append(metricId(metric));
        }
    }
    return ids.join(QLatin1Char(','));
}

QVector<Metric> defaultCustomMetrics()
{
    return { Metric::Fps,     Metric::OnePercentLow, Metric::Latency,
             Metric::Bitrate, Metric::Loss,          Metric::FrameTimeGraph };
}

Grade gradeAbove(double value, double warn, double bad)
{
    if (!(value >= 0)) {
        return Grade::Neutral;
    }
    if (value > bad) {
        return Grade::Bad;
    }
    if (value > warn) {
        return Grade::Warn;
    }
    return Grade::Good;
}

Grade gradeFps(double fps, int targetFps, const Thresholds& thresholds)
{
    if (targetFps <= 0 || !(fps > 0)) {
        return Grade::Neutral;
    }
    if (fps < targetFps * thresholds.fpsBadRatio) {
        return Grade::Bad;
    }
    if (fps < targetFps * thresholds.fpsWarnRatio) {
        return Grade::Warn;
    }
    return Grade::Good;
}

Grade gradeLow(double lowFps, double fps, const Thresholds& thresholds)
{
    if (!(fps > 0) || !(lowFps > 0)) {
        return Grade::Neutral;
    }
    if (lowFps < fps * thresholds.lowBadRatio) {
        return Grade::Bad;
    }
    if (lowFps < fps * thresholds.lowWarnRatio) {
        return Grade::Warn;
    }
    return Grade::Good;
}

Grade gradeFrameTime(double frameTimeMs, int targetFps, const Thresholds& thresholds)
{
    if (targetFps <= 0 || !(frameTimeMs > 0)) {
        return Grade::Neutral;
    }
    const double targetMs = 1000.0 / targetFps;
    return gradeAbove(frameTimeMs, targetMs * thresholds.spikeWarnRatio,
                      targetMs * thresholds.spikeBadRatio);
}

Grade gradePresent(double prePresentMs, double presentMs, int targetFps,
                   const Thresholds& thresholds)
{
    if (targetFps <= 0 || !(prePresentMs >= 0) || !(presentMs >= 0)) {
        return Grade::Neutral;
    }
    const double targetMs = 1000.0 / targetFps;
    return gradeAbove(prePresentMs + presentMs, targetMs * thresholds.presentWarnRatio,
                      targetMs * thresholds.presentBadRatio);
}

int clampScalePercent(int percent)
{
    return std::clamp(percent, MinScalePercent, MaxScalePercent);
}

int clampOpacityPercent(int percent)
{
    return std::clamp(percent, MinOpacityPercent, MaxOpacityPercent);
}

QString anchorId(Anchor anchor)
{
    const int index = static_cast<int>(anchor);
    if (index < 0 || index > static_cast<int>(Anchor::BottomRight)) {
        return QLatin1String(k_AnchorIds[0]);
    }
    return QLatin1String(k_AnchorIds[index]);
}

Anchor anchorFromId(const QString& id, Anchor fallback)
{
    for (int i = 0; i <= static_cast<int>(Anchor::BottomRight); i++) {
        if (id.compare(QLatin1String(k_AnchorIds[i]), Qt::CaseInsensitive) == 0) {
            return static_cast<Anchor>(i);
        }
    }
    return fallback;
}

QPoint anchoredPosition(Anchor anchor, const QRect& parent, const QSize& size, int margin)
{
    const int index = static_cast<int>(anchor);
    const int column = index % 3;
    const int row = index / 3;

    int x;
    switch (column) {
    case 0:
        x = parent.x() + margin;
        break;
    case 1:
        x = parent.x() + (parent.width() - size.width()) / 2;
        break;
    default:
        x = parent.x() + parent.width() - size.width() - margin;
        break;
    }

    int y;
    switch (row) {
    case 0:
        y = parent.y();
        break;
    case 1:
        y = parent.y() + (parent.height() - size.height()) / 2;
        break;
    default:
        y = parent.y() + parent.height() - size.height() - margin;
        break;
    }

    return QPoint(x, y);
}

Anchor nearestAnchor(const QRect& parent, const QPoint& point)
{
    if (parent.width() <= 0 || parent.height() <= 0) {
        return Anchor::TopLeft;
    }

    const int dx = std::clamp(point.x() - parent.x(), 0, parent.width() - 1);
    const int dy = std::clamp(point.y() - parent.y(), 0, parent.height() - 1);
    const int column = std::min(dx * 3 / parent.width(), 2);
    const int row = std::min(dy * 3 / parent.height(), 2);
    return static_cast<Anchor>(row * 3 + column);
}

QPoint lightlySnappedPosition(const QPoint& topLeft, const QSize& size, const QRect& bounds,
                              int threshold)
{
    if (!bounds.isValid()) {
        return topLeft;
    }
    const auto placeAxis = [threshold](int value, int start, int span, int extent) {
        const int end = start + std::max(span - extent, 0);
        value = std::clamp(value, start, end);
        int result = value;
        int closest = threshold + 1;
        for (int target : { start, start + (end - start) / 2, end }) {
            const int distance = std::abs(value - target);
            if (distance < closest) {
                closest = distance;
                result = target;
            }
        }
        return result;
    };
    return QPoint(placeAxis(topLeft.x(), bounds.x(), bounds.width(), size.width()),
                  placeAxis(topLeft.y(), bounds.y(), bounds.height(), size.height()));
}

namespace {

// rect moved to lie inside area; its top-left wins when it is too big
QPoint fitInto(const QRect& rect, const QRect& area)
{
    int x = std::min(rect.x(), area.x() + area.width() - rect.width());
    int y = std::min(rect.y(), area.y() + area.height() - rect.height());
    x = std::max(x, area.x());
    y = std::max(y, area.y());
    return QPoint(x, y);
}

} // namespace

QPoint detachedWindowPosition(const std::optional<QPoint>& saved, const QSize& size,
                              const QVector<QRect>& screens, const QRect& streamScreen, int margin)
{
    if (saved) {
        const QRect rect(*saved, size);
        QRect best;
        QRect bestOverlap;
        for (const QRect& screen : screens) {
            const QRect overlap = rect.intersected(screen);
            if (qint64(overlap.width()) * overlap.height() >
                qint64(bestOverlap.width()) * bestOverlap.height()) {
                best = screen;
                bestOverlap = overlap;
            }
        }
        if (bestOverlap.width() >= std::min(size.width(), DetachedMinVisible) &&
            bestOverlap.height() >= std::min(size.height(), DetachedMinVisible) &&
            !bestOverlap.isEmpty()) {
            return fitInto(rect, best);
        }
    }

    QRect area = streamScreen;
    for (const QRect& screen : screens) {
        if (!screen.isEmpty() && !screen.intersects(streamScreen)) {
            area = screen;
            break;
        }
    }
    if (area.isEmpty() && !screens.isEmpty()) {
        area = screens.first();
    }
    const QPoint position = area.topLeft() + QPoint(margin, margin);
    return area.isEmpty() ? position : fitInto(QRect(position, size), area);
}

QString displayKey(const QString& screenName)
{
    QString key;
    key.reserve(screenName.size());
    for (const QChar c : screenName) {
        key.append(c.isLetterOrNumber() || c == QLatin1Char('-') ? c : QLatin1Char('_'));
    }
    if (key.isEmpty()) {
        key = QStringLiteral("default");
    }
    return key;
}

QString formatDuration(qint64 seconds)
{
    seconds = std::max<qint64>(seconds, 0);
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(secs, 2, 10, QLatin1Char('0'));
}

} // namespace StatsOverlay
