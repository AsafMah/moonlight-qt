#include "overlaystatswindow.h"
#include "overlaystatsfonts.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace StatsOverlay;

QSize OverlayStatsWindow::layoutStrip(QPainter* painter, int requestedWidth) const
{
    const QColor foreground("#e6e6e6"), secondary("#b8bbc0");
    const QColor white("#ffffff"), cyan("#20d5e5"), green("#42df63");
    const QColor amber("#ffcf38"), purple("#d6aaff"), grey("#c0c4cb"), red("#ff9fae");
    const QFont valueFont = stripFont(), labelFont = stripFont(18, true);
    const QFont smallFont = stripFont(13);
    const QFont compactLabelFont = stripFont(14);
    const QFontMetricsF values(valueFont), labels(labelFont), small(smallFont);
    const QFontMetricsF compactLabels(compactLabelFont);
    const int rowHeight = qCeil(std::max(values.height(), labels.height())) + 4;

    struct Item
    {
        QString label;
        QString value;
        QString unit;
        QColor color;
        std::optional<Metric> metric;
        QString tooltip;
        qreal width = 0;
        QPointF position;
        bool graph = false;
        bool heading = false;
        QString inlineLabel;
    };
    struct Group
    {
        QVector<Item> items;
        bool newLine = false;
        qreal width = 0;
    };
    QVector<Group> groups;
    auto group = [&](bool newLine = false) -> Group& {
        groups.append({ {}, newLine, 0 });
        return groups.last();
    };
    auto add = [&](Group& target, Metric metric, QColor color, const QString& label = QString()) {
        const Cell cell = cellFor(metric);
        QString value = cell.value;
        if (metric == Metric::Fps && m_Preset != Preset::Minimal && m_Snapshot.valid) {
            value = QStringLiteral("%1/%2/%3")
                        .arg(number(m_Snapshot.receivedFps, 0), number(m_Snapshot.decodedFps, 0),
                             number(m_Snapshot.renderedFps, 0));
        }
        const QString tooltip = value;
        QString unit;
        for (const QString& suffix : { QStringLiteral(" Mbps"), QStringLiteral(" ms") }) {
            if (value.endsWith(suffix)) {
                unit = suffix.trimmed();
                value.chop(suffix.size());
                break;
            }
        }
        target.items.append(
            { label.isEmpty() ? (cell.shortLabel.isEmpty() ? cell.label : cell.shortLabel) : label,
              value,
              unit,
              color,
              metric,
              tooltip,
              0,
              {},
              false,
              target.items.isEmpty() });
    };
    const Cell latency = cellFor(Metric::Latency);
    const QColor latencyColor = latency.grade == Grade::Bad    ? red
                                : latency.grade == Grade::Warn ? amber
                                                               : green;
    auto& fps = group();
    add(fps, Metric::Fps, white);
    add(fps, Metric::OnePercentLow, white);
    if (m_Preset == Preset::Minimal) {
        add(fps, Metric::Latency, latencyColor);
    } else {
        add(fps, Metric::PacerDrops, white);
        if (m_Preset == Preset::Detailed) {
            fps.items.append({ tr("0.1% low"),
                               m_Snapshot.valid && m_Snapshot.frameTimes.samples > 0
                                   ? number(m_Snapshot.frameTimes.pointOnePercentLowFps, 0)
                                   : dash(),
                               {},
                               white,
                               {},
                               {} });
        }
        // The graph identifies frame times, not synthetic minimum/maximum FPS.
        fps.items.append(
            { tr("Frame time"), {}, {}, green, Metric::FrameTimeGraph, {}, 0, {}, true });
        auto& net = group();
        add(net, Metric::Rtt, cyan);
        add(net, Metric::Jitter, cyan);
        add(net, Metric::Loss, cyan);
        auto& lat = group(m_Preset == Preset::Detailed);
        add(lat, Metric::Latency, latencyColor);
        add(lat, Metric::Host, latencyColor);
        if (m_Preset == Preset::Detailed && m_Snapshot.latency.hasNetwork) {
            lat.items.append({ tr("Network"),
                               m_Snapshot.valid ? number(m_Snapshot.latency.networkMs, 1) : dash(),
                               QStringLiteral("ms"),
                               latencyColor,
                               {},
                               {} });
        }
        add(lat, Metric::Decode, latencyColor);
        add(lat, Metric::Queue, latencyColor);
        add(lat, Metric::Render, latencyColor);
        add(group(), Metric::Bitrate, purple);
        add(group(m_Preset == Preset::Detailed), Metric::Resolution, grey);
        if (m_Preset == Preset::Detailed) {
            auto& pipeline = group();
            QString text = m_Snapshot.decoder;
            if (!m_Snapshot.renderer.isEmpty()) {
                if (!text.isEmpty()) {
                    text += QStringLiteral(" · ");
                }
                text += m_Snapshot.renderer;
            }
            pipeline.items.append(
                { {}, m_Snapshot.valid && !text.isEmpty() ? text : dash(), {}, grey, {}, {} });
            add(group(), Metric::DisplayHz, amber);
            if (m_Snapshot.presentTiming.measured) {
                add(group(true), Metric::Present, green);
            }
            if (m_Snapshot.videoPackets > 0) {
                add(group(), Metric::Fec, cyan);
            }
            add(group(), Metric::Abr, purple);
            add(group(), Metric::SessionTime, grey);
        }
    }

    constexpr qreal gap = 4;
    constexpr qreal separation = 13;
    constexpr qreal padding = 4;
    qreal widestItem = 0, widestGroup = 0;
    for (auto& g : groups) {
        for (auto& item : g.items) {
            item.inlineLabel = item.label;
            if (item.graph) {
                item.inlineLabel = QStringLiteral("FT");
            } else if (item.metric) {
                switch (*item.metric) {
                case Metric::OnePercentLow:
                    item.inlineLabel = QStringLiteral("↓");
                    break;
                case Metric::Host:
                    item.inlineLabel = QStringLiteral("h");
                    break;
                case Metric::Decode:
                    item.inlineLabel = QStringLiteral("d");
                    break;
                case Metric::Queue:
                    item.inlineLabel = QStringLiteral("q");
                    break;
                case Metric::Render:
                    item.inlineLabel = QStringLiteral("r");
                    break;
                case Metric::Rtt:
                    item.inlineLabel = QStringLiteral("NET");
                    break;
                case Metric::Latency:
                    item.inlineLabel = QStringLiteral("LAT");
                    break;
                case Metric::Bitrate:
                    item.inlineLabel = QStringLiteral("RATE");
                    break;
                case Metric::Resolution:
                    item.inlineLabel = QStringLiteral("RES");
                    break;
                default:
                    break;
                }
            }
            const qreal valueWidth = std::max(values.horizontalAdvance(item.value),
                                              values.horizontalAdvance(stableText(item.value)));
            const qreal labelWidth =
                (item.heading ? labels : compactLabels).horizontalAdvance(item.inlineLabel);
            item.width = labelWidth + (item.inlineLabel.isEmpty() ? 0 : gap) + valueWidth;
            if (!item.unit.isEmpty()) {
                item.width += small.horizontalAdvance(item.unit) + 1;
            }
            if (item.graph) {
                item.width += 128;
            }
            widestItem = std::max(
                widestItem, std::max(compactLabels.horizontalAdvance(item.label),
                                     item.graph ? qreal(128)
                                                : valueWidth + small.horizontalAdvance(item.unit) +
                                                      (item.unit.isEmpty() ? 0 : 1)));
            g.width += item.width + gap;
        }
        g.width -= gap;
        widestGroup = std::max(widestGroup, g.width);
    }
    const int maxWidth = m_MaximumBodySize.isValid() ? m_MaximumBodySize.width() : MaxPanelWidth;
    const int desired = requestedWidth > 0                                    ? requestedWidth
                        : m_BodySize.isValid() && m_Preset != Preset::Minimal ? m_BodySize.width()
                        : m_Preset == Preset::Minimal  ? qCeil(widestGroup + padding * 2)
                        : m_Preset == Preset::Detailed ? 720
                                                       : defaultPanelWidth(m_Preset);
    const qreal limit = std::max(widestItem + padding * 2, qreal(std::min(desired, maxWidth)));
    const bool stacked = limit < widestGroup + padding * 2;
    qreal x = padding, used = padding;
    int y = 0;
    const int height = stacked ? rowHeight + qCeil(compactLabels.height()) : rowHeight;
    for (auto& g : groups) {
        if (!stacked && x > padding && (g.newLine || x + separation + g.width + padding > limit)) {
            used = std::max(used, x);
            x = padding;
            y += height;
        }
        if (!stacked && x > padding) {
            x += separation;
        }
        for (auto& item : g.items) {
            if (stacked) {
                item.width = std::max(compactLabels.horizontalAdvance(item.label),
                                      std::max(values.horizontalAdvance(item.value),
                                               values.horizontalAdvance(stableText(item.value))) +
                                          small.horizontalAdvance(item.unit) +
                                          (item.unit.isEmpty() ? 0 : 1));
                if (item.graph) {
                    item.width = std::max(item.width, qreal(128));
                }
                if (x > padding && x + item.width + padding > limit) {
                    used = std::max(used, x);
                    x = padding;
                    y += height;
                }
            }
            item.position = QPointF(x, y);
            x += item.width + gap;
        }
        x -= gap;
        if (stacked) {
            x += separation;
        }
    }
    used = std::max(used, x);
    QSize result(qCeil(used + padding), y + height);
    if (m_Editing) {
        // Even Minimal must leave room for the full preset-cycle button.
        const int controlsWidth =
            QFontMetrics(m_LabelFont).horizontalAdvance(tr("Details: %1").arg(tr("Detailed"))) +
            PanelSpacing::Pad * 4 + PanelSpacing::AccentBar;
        result.setWidth(std::max(result.width(), std::min(controlsWidth, maxWidth)));
    }
    if (m_Editing && requestedWidth == 0 && m_MaximumBodySize.isValid()) {
        int width = qCeil(limit);
        QSize fitted = result;
        while (fitted.height() + layoutEditControls(nullptr, 0, fitted.width()) +
                       editHintHeight(fitted.width()) >
                   m_MaximumBodySize.height() &&
               width < maxWidth) {
            width = std::min(width + 32, maxWidth);
            fitted = layoutStrip(nullptr, width);
        }
        if (width > qCeil(limit)) {
            return layoutStrip(painter, width);
        }
    }
    if (painter == nullptr) {
        return result;
    }
    auto draw = [&](const QString& text, QPointF point, const QFont& font, QColor color) {
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(point + QPointF(0, QFontMetricsF(font).ascent()), text);
    };
    for (const auto& g : groups) {
        if (!stacked && !g.items.isEmpty() && g.items.first().position.x() > padding) {
            draw(QStringLiteral("|"), g.items.first().position - QPointF(separation - 3, -1),
                 valueFont, QColor("#72767c"));
        }
        for (const auto& item : g.items) {
            const QPointF point = item.position;
            const QFont& itemFont = !stacked && item.heading ? labelFont : compactLabelFont;
            const QString& label = stacked ? item.label : item.inlineLabel;
            draw(label, point + QPointF(0, item.heading || stacked ? 1 : 4), itemFont,
                 item.heading || stacked ? item.color : secondary);
            const qreal valueX =
                stacked || label.isEmpty()
                    ? point.x()
                    : point.x() + QFontMetricsF(itemFont).horizontalAdvance(label) + gap;
            const qreal valueY = point.y() + (stacked ? compactLabels.height() + 1 : 1);
            draw(item.value, QPointF(valueX, valueY), valueFont, foreground);
            if (!item.unit.isEmpty()) {
                draw(item.unit, QPointF(valueX + values.horizontalAdvance(item.value) + 1, valueY),
                     smallFont, secondary);
            }
            if (item.metric) {
                m_HitRegions.append(
                    { *item.metric,
                      QRect(qFloor(point.x()), qFloor(point.y()), qCeil(item.width), height),
                      item.tooltip });
            }
            if (item.graph) {
                const QRectF graph(stacked ? point.x() : valueX, valueY + 2, 90,
                                   std::max(8, rowHeight - 8));
                const auto& samples = m_Snapshot.frameTimeHistoryMs;
                if (m_Snapshot.valid && samples.size() > 1) {
                    const double budget =
                        m_Snapshot.targetFps > 0 ? 1000.0 / m_Snapshot.targetFps : 16.7;
                    double ceiling = budget * 2;
                    double minimum = std::numeric_limits<double>::infinity(), maximum = 0;
                    for (float sample : samples) {
                        if (std::isfinite(sample)) {
                            ceiling = std::max(ceiling, double(sample));
                            minimum = std::min(minimum, double(sample));
                            maximum = std::max(maximum, double(sample));
                        }
                    }
                    auto position = [&](size_t index) {
                        return QPointF(graph.left() + graph.width() * index / (samples.size() - 1),
                                       graph.bottom() -
                                           graph.height() *
                                               std::clamp(double(samples[index]), 0.0, ceiling) /
                                               ceiling);
                    };
                    for (size_t i = 1; i < samples.size(); ++i) {
                        if (!std::isfinite(samples[i]) || !std::isfinite(samples[i - 1])) {
                            continue;
                        }
                        painter->setPen(QPen(samples[i] > budget * 1.25 ? red : green, 1));
                        painter->drawLine(position(i - 1), position(i));
                    }
                    if (std::isfinite(minimum)) {
                        draw(number(maximum, 1), QPointF(graph.right() + 3, valueY), stripFont(10),
                             red);
                        draw(number(minimum, 1), QPointF(graph.right() + 3, valueY + 13),
                             stripFont(10), green);
                        m_HitRegions.last().value =
                            QStringLiteral("%1–%2 ms").arg(number(minimum, 1), number(maximum, 1));
                    }
                }
            }
        }
    }
    return result;
}
