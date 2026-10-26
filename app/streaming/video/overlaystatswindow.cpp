#include "overlaystatswindow.h"
#include "overlaystatsfonts.h"

#include <QCoreApplication>
#include <QCursor>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace StatsOverlay;

namespace {

// app/gui/theme/Theme.qml tokens. This window is painted with QPainter and
// cannot reach the QML singleton, so they are copied, as in overlaytoast.cpp.
const QColor kSurface(0x17, 0x1A, 0x20);   // Theme.surface
const QColor kSurface2(0x1F, 0x23, 0x2B);  // Theme.surface2
const QColor kLine(0x2B, 0x30, 0x38);      // Theme.line
const QColor kAccent(0x39, 0xC5, 0xBB);    // Theme.accent
const QColor kText(0xEE, 0xF0, 0xEC);      // Theme.text
const QColor kTextDim(0xAE, 0xB3, 0xAB);   // Theme.textDim
const QColor kTextFaint(0x7E, 0x85, 0x8E); // Theme.textFaint
const QColor kDanger(0xFF, 0x87, 0x6F);    // Theme.danger
const QColor kAcid(0xC8, 0xFF, 0x4D);      // Theme.acid
const QColor kShadow(0, 0, 0, 0x8C);       // Theme.shadowColor
// Theme.qml has no warning color; amber sits between accent and danger
const QColor kWarn(0xFF, 0xC8, 0x57);
// Latency stages, chosen to stay apart from the grade colors
const QColor kStageHost(0x8A, 0x9B, 0xFF);
const QColor kStageRender(0xD6, 0x8C, 0xFF);

const int kShadowOffset = 4;
const int kAccentBar = PanelSpacing::AccentBar;
const int kPad = PanelSpacing::Pad;
const int kCellPadX = PanelSpacing::CellPadX;
const int kLabelGap = PanelSpacing::LabelGap;
// Width of the band along the edges that resizes in edit mode, in window pixels
const int kResizeMargin = 6;

QColor gradeColor(Grade grade)
{
    switch (grade) {
    case Grade::Good:
        return kAccent;
    case Grade::Warn:
        return kWarn;
    case Grade::Bad:
        return kDanger;
    case Grade::Neutral:
    default:
        return kText;
    }
}

QString dash()
{
    return QStringLiteral("–");
}

QString number(double value, int decimals)
{
    if (!std::isfinite(value)) {
        return dash();
    }
    return QString::number(value, 'f', decimals);
}

QString milliseconds(double value, int decimals = 1)
{
    return number(value, decimals) + QStringLiteral(" ms");
}

const Thresholds& thresholds()
{
    static const Thresholds defaults;
    return defaults;
}

} // namespace

OverlayStatsWindow::OverlayStatsWindow(QWindow* parent, OverlayWindowMode windowMode)
    : OverlayRasterWindow(parent, windowMode), m_WindowMode(windowMode)
{
    Q_ASSERT(m_WindowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(m_WindowMode, true));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    const OverlayFonts fonts = overlayFonts();
    m_LabelFont = fonts.label;
    m_ValueFont = fonts.value;
    m_BigFont = fonts.big;

    m_Metrics = defaultCustomMetrics();
}

void OverlayStatsWindow::setStats(const StatsSnapshot& snapshot, const Extras& extras)
{
    m_Snapshot = snapshot;
    m_Extras = extras;
}

void OverlayStatsWindow::setPreset(Preset preset)
{
    m_Preset = preset == Preset::Off ? Preset::Compact : preset;
}

void OverlayStatsWindow::setMetrics(const QVector<Metric>& metrics)
{
    m_Metrics = metrics;
}

void OverlayStatsWindow::setScalePercent(int percent)
{
    m_ScalePercent = clampScalePercent(percent);
}

void OverlayStatsWindow::setOpacityPercent(int percent)
{
    m_OpacityPercent = clampOpacityPercent(percent);
}

void OverlayStatsWindow::setStandalone(bool standalone)
{
    m_Standalone = standalone;
    if (standalone) {
        // An ordinary window, so it can sit on any screen and shows in the
        // taskbar, but clicking it never takes focus from the stream
        setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                 Qt::WindowDoesNotAcceptFocus);
        setTitle(tr("Moonlight performance overlay"));
    } else {
        setFlags(OverlayWindowPolicy::flags(m_WindowMode, !m_Editing));
    }
}

void OverlayStatsWindow::configureWindow(QWindow* parent, OverlayWindowMode windowMode,
                                         bool standalone)
{
    m_WindowMode = windowMode;
    m_Standalone = standalone;
    const Qt::WindowFlags flags = standalone
                                      ? Qt::Window | Qt::FramelessWindowHint |
                                            Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus
                                      : OverlayWindowPolicy::flags(windowMode, !m_Editing);
    reconfigureSurface(parent, windowMode, flags);
    if (standalone) {
        setTitle(tr("Moonlight performance overlay"));
    }
}

void OverlayStatsWindow::setEditing(bool editing)
{
    if (m_Editing == editing) {
        return;
    }

    m_Editing = editing;
    m_Dragging = false;
    m_ResizeEdges = 0;
    if (!editing) {
        unsetCursor();
    }
    if (m_Standalone) {
        // Always takes input, so the flags stay as they are
        return;
    }

    // Input transparency is a window flag, which some platforms only apply
    // when the native window is recreated, so hide around the change.
    const bool wasVisible = isVisible();
    if (wasVisible) {
        hide();
    }
    setFlags(OverlayWindowPolicy::flags(m_WindowMode, !editing));
    if (wasVisible) {
        show();
        raise();
    }
}

QSize OverlayStatsWindow::preferredSize() const
{
    const QSize body = layoutBody(nullptr);
    return QSize(static_cast<int>(std::ceil((body.width() + kShadowOffset) * scale())),
                 static_cast<int>(std::ceil((body.height() + kShadowOffset) * scale())));
}

void OverlayStatsWindow::repaintNow()
{
    if (!isExposed()) {
        return;
    }

    update(QRect(QPoint(), size()));
    QEvent event(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(this, &event);
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

OverlayStatsWindow::Cell OverlayStatsWindow::cellFor(Metric metric) const
{
    const StatsSnapshot& s = m_Snapshot;
    const Thresholds& t = thresholds();
    Cell cell;
    cell.metric = metric;

    if (!s.valid) {
        cell.value = dash();
    }

    switch (metric) {
    case Metric::Resolution:
        cell.label = tr("Stream");
        cell.shortLabel = tr("Res", "Short form of the Stream (resolution) label");
        if (s.valid) {
            cell.value = QStringLiteral("%1x%2@%3 %4")
                             .arg(s.width)
                             .arg(s.height)
                             .arg(qRound(s.totalFps))
                             .arg(s.codec);
        }
        break;
    case Metric::Fps:
        cell.label = tr("FPS");
        if (s.valid) {
            cell.value = number(s.renderedFps, 0);
            cell.grade = gradeFps(s.renderedFps, s.targetFps, t);
        }
        break;
    case Metric::OnePercentLow:
        cell.label = tr("1% low");
        cell.value = s.frameTimes.samples > 0 ? number(s.frameTimes.onePercentLowFps, 0) : dash();
        cell.grade = gradeLow(s.frameTimes.onePercentLowFps, s.frameTimes.fps, t);
        break;
    case Metric::Latency:
        cell.label = tr("Latency");
        cell.shortLabel = tr("Lat", "Short form of the Latency label");
        if (s.valid) {
            cell.value = QStringLiteral("~") + milliseconds(s.latency.totalMs(), 0);
            cell.grade = gradeAbove(s.latency.totalMs(), t.latencyWarnMs, t.latencyBadMs);
        }
        break;
    case Metric::Bitrate:
        cell.label = tr("Bitrate");
        cell.shortLabel = tr("Rate", "Short form of the Bitrate label");
        if (s.valid) {
            cell.value = number(s.bitrateMbps, 1);
            if (m_Extras.targetBitrateKbps > 0) {
                cell.value +=
                    QStringLiteral(" / ") + number(m_Extras.targetBitrateKbps / 1000.0, 0);
            }
            cell.value += QStringLiteral(" Mbps");
        }
        break;
    case Metric::Loss:
        cell.label = tr("Loss");
        if (s.valid) {
            cell.value = number(s.networkLossPercent, 2) + QStringLiteral("%");
            cell.grade = gradeAbove(s.networkLossPercent, t.lossWarnPercent, t.lossBadPercent);
        }
        break;
    case Metric::Rtt:
        cell.label = tr("RTT");
        if (s.valid) {
            if (s.hasRtt) {
                cell.value = QStringLiteral("%1 ± %2 ms").arg(s.rttMs).arg(s.rttVarianceMs);
                cell.grade = gradeAbove(s.rttMs, t.rttWarnMs, t.rttBadMs);
            } else {
                cell.value = dash();
            }
        }
        break;
    case Metric::Jitter:
        cell.label = tr("Jitter");
        cell.shortLabel = tr("Jit", "Short form of the Jitter label");
        if (s.valid) {
            cell.value = milliseconds(s.jitterMs, 2);
            cell.grade = gradeAbove(s.jitterMs, t.jitterWarnMs, t.jitterBadMs);
        }
        break;
    case Metric::Host:
        cell.label = tr("Host");
        if (s.valid) {
            cell.value = s.latency.hasHost ? milliseconds(s.latency.hostMs) : dash();
        }
        break;
    case Metric::Decode:
        cell.label = tr("Decode");
        cell.shortLabel = tr("Dec", "Short form of the Decode label");
        if (s.valid) {
            cell.value = milliseconds(s.latency.decodeMs);
            cell.grade = gradeAbove(s.latency.decodeMs, t.decodeWarnMs, t.decodeBadMs);
        }
        break;
    case Metric::Queue:
        cell.label = tr("Queue");
        if (s.valid) {
            cell.value = s.latency.hasQueue ? milliseconds(s.latency.queueMs) : dash();
        }
        break;
    case Metric::Render:
        cell.label = tr("Render");
        cell.shortLabel = tr("Rend", "Short form of the Render label");
        if (s.valid) {
            cell.value = milliseconds(s.latency.renderMs);
        }
        break;
    case Metric::Present:
        cell.label = tr("Pre-Present / Present");
        cell.shortLabel = tr("Phases");
        cell.value = dash();
        if (s.valid && s.presentTiming.measured) {
            cell.value = QStringLiteral("%1 / %2 ms")
                             .arg(number(s.presentTiming.prePresentMs, 1),
                                  number(s.presentTiming.presentMs, 1));
            cell.grade = gradePresent(s.presentTiming.prePresentMs, s.presentTiming.presentMs,
                                      s.targetFps, t);
        }
        break;
    case Metric::Fec:
        cell.label = tr("FEC");
        if (s.valid) {
            const double packets = double(s.videoPackets) + double(s.fecPackets);
            cell.value = packets > 0
                             ? tr("%1% parity").arg(number(s.fecPackets * 100.0 / packets, 0))
                             : dash();
            // Only when moonlight-common-c reports FEC results, see
            // VideoStatsBridge::readRtpCounters()
            if (s.fecRecoveredFrames >= 0 && s.fecUnrecoverableFrames >= 0) {
                cell.value += QStringLiteral(" · ") + tr("%1 recovered, %2 lost")
                                                          .arg(s.fecRecoveredFrames)
                                                          .arg(s.fecUnrecoverableFrames);
                cell.grade = s.fecUnrecoverableFrames > 0 ? Grade::Warn : Grade::Good;
            }
        }
        break;
    case Metric::Abr:
        cell.label = tr("ABR");
        cell.value = m_Extras.abrEnabled ? tr("On") : tr("Off");
        break;
    case Metric::PacerDrops:
        cell.label = tr("Pacer drops");
        cell.shortLabel = tr("Drops", "Short form of the Pacer drops label");
        if (s.valid) {
            cell.value = number(s.pacerDropPercent, 1) + QStringLiteral("%");
            cell.grade =
                gradeAbove(s.pacerDropPercent, t.pacerDropWarnPercent, t.pacerDropBadPercent);
        }
        break;
    case Metric::DisplayHz:
        cell.label = tr("Display");
        cell.shortLabel = tr("Disp", "Short form of the Display label");
        if (m_Extras.displayHz > 0) {
            cell.value = QStringLiteral("%1 Hz").arg(number(m_Extras.displayHz, 0));
            if (s.targetFps > 0) {
                cell.value += QStringLiteral(" / %1 fps").arg(s.targetFps);
                // A display slower than the stream has to drop frames
                cell.grade = m_Extras.displayHz + 0.5 < s.targetFps ? Grade::Warn : Grade::Good;
            }
            if (m_Extras.vrrCapFps >= 0) {
                // In VRR mode presents are capped just below the refresh rate
                cell.value += QStringLiteral(" · VRR");
                if (m_Extras.vrrCapFps > 0 && s.targetFps > m_Extras.vrrCapFps) {
                    cell.grade = Grade::Warn;
                }
            }
        } else {
            cell.value = dash();
        }
        break;
    case Metric::SessionTime:
        cell.label = tr("Time");
        cell.value = formatDuration(static_cast<qint64>(s.sessionDurationUs / 1000000));
        break;
    case Metric::FrameTimeGraph:
    case Metric::LatencyBar:
        break;
    }

    return cell;
}

QVector<OverlayStatsWindow::Cell> OverlayStatsWindow::cellsFor(const QVector<Metric>& metrics) const
{
    QVector<Cell> cells;
    for (Metric metric : metrics) {
        if (metric == Metric::FrameTimeGraph || metric == Metric::LatencyBar) {
            continue;
        }
        if (metric == Metric::Present && m_Preset == Preset::Detailed &&
            !m_Snapshot.presentTiming.measured) {
            continue;
        }
        cells.append(cellFor(metric));
    }
    return cells;
}

QVector<Metric> OverlayStatsWindow::panelMetrics() const
{
    if (m_Preset == Preset::Custom) {
        return m_Metrics;
    }
    if (m_Preset == Preset::Compact) {
        return compactMetrics();
    }

    QVector<Metric> metrics = { Metric::Rtt,  Metric::Jitter,     Metric::Bitrate,
                                Metric::Loss, Metric::PacerDrops, Metric::DisplayHz,
                                Metric::Abr,  Metric::SessionTime };
    if (m_Snapshot.presentTiming.measured) {
        metrics.append(Metric::Present);
    }
    // FEC only when the transport reports it
    if (m_Snapshot.videoPackets > 0) {
        metrics.insert(4, Metric::Fec);
    }
    return metrics;
}

bool OverlayStatsWindow::panelHas(Metric metric) const
{
    if (metric == Metric::Present && m_Preset == Preset::Detailed &&
        !m_Snapshot.presentTiming.measured) {
        return false;
    }
    return m_Preset == Preset::Detailed || m_Metrics.contains(metric);
}

// ---------------------------------------------------------------------------
// Layout and painting
// ---------------------------------------------------------------------------

QSize OverlayStatsWindow::layoutMinimal(QPainter* painter) const
{
    const QFontMetrics valueMetrics(m_ValueFont);
    const QFontMetrics labelMetrics(m_LabelFont);
    const int height = kPad * 2 + valueMetrics.height() - 4;

    const Cell cells[] = { cellFor(Metric::Fps), cellFor(Metric::OnePercentLow),
                           cellFor(Metric::Latency) };

    int x = kAccentBar + kPad;
    bool first = true;
    for (const Cell& cell : cells) {
        if (!first) {
            x += kCellPadX;
        }
        first = false;

        const int valueWidth = valueMetrics.horizontalAdvance(cell.value);
        const int labelWidth = labelMetrics.horizontalAdvance(cell.label);
        if (painter != nullptr) {
            painter->setFont(m_ValueFont);
            painter->setPen(gradeColor(cell.grade));
            painter->drawText(QRect(x, 0, valueWidth, height), Qt::AlignLeft | Qt::AlignVCenter,
                              cell.value);
            painter->setFont(m_LabelFont);
            painter->setPen(kTextDim);
            painter->drawText(QRect(x + valueWidth + kLabelGap, 0, labelWidth, height),
                              Qt::AlignLeft | Qt::AlignVCenter, cell.label);
            m_HitRegions.append({ cell.metric,
                                  QRect(x, 0, valueWidth + kLabelGap + labelWidth, height),
                                  cell.value });
        }
        x += valueWidth + kLabelGap + labelWidth;
    }

    return QSize(x + kPad, height);
}

QSize OverlayStatsWindow::layoutCompact(QPainter* painter) const
{
    const QFontMetrics valueMetrics(m_ValueFont);
    const QFontMetrics labelMetrics(m_LabelFont);
    const int height = kPad + labelMetrics.height() + kLabelGap + valueMetrics.height() + kPad;

    const QVector<Cell> cells = cellsFor({ Metric::Resolution, Metric::Fps, Metric::OnePercentLow,
                                           Metric::Latency, Metric::Bitrate, Metric::Loss });

    int x = kAccentBar;
    for (int i = 0; i < cells.size(); i++) {
        const Cell& cell = cells[i];
        const int width = std::max(valueMetrics.horizontalAdvance(cell.value),
                                   labelMetrics.horizontalAdvance(cell.label)) +
                          kCellPadX * 2;
        if (painter != nullptr) {
            if (i > 0) {
                painter->fillRect(QRect(x, kPad, 1, height - kPad * 2), kLine);
            }
            painter->setFont(m_LabelFont);
            painter->setPen(kTextFaint);
            painter->drawText(QRect(x + kCellPadX, kPad, width, labelMetrics.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, cell.label);
            painter->setFont(m_ValueFont);
            painter->setPen(gradeColor(cell.grade));
            painter->drawText(QRect(x + kCellPadX, kPad + labelMetrics.height() + kLabelGap, width,
                                    valueMetrics.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, cell.value);
            m_HitRegions.append({ cell.metric, QRect(x, 0, width, height), cell.value });
        }
        x += width;
    }

    return QSize(x, height);
}

PanelContent OverlayStatsWindow::panelContent(QVector<Cell>& cells) const
{
    PanelContent content;
    content.compactStrip = m_Preset == Preset::Compact;

    if (m_Preset == Preset::Detailed) {
        content.header = true;
        const Cell fps = cellFor(Metric::Fps);
        content.fpsValue = fps.value;
        content.fpsLabel = fps.label;

        const Cell low = cellFor(Metric::OnePercentLow);
        QString lows = low.label + QStringLiteral(" ") + low.value;
        if (m_Snapshot.frameTimes.samples > 0) {
            lows += QStringLiteral("  ") + tr("0.1% low") + QStringLiteral(" ") +
                    number(m_Snapshot.frameTimes.pointOnePercentLowFps, 0);
        }
        QString pipeline = m_Snapshot.decoder;
        if (!m_Snapshot.renderer.isEmpty()) {
            pipeline += QStringLiteral(" · ") + m_Snapshot.renderer;
        }
        // Spelled out: Qt 5 finds assigning a braced list to a QStringList ambiguous
        content.headerLines = QStringList{ lows, cellFor(Metric::Resolution).value, pipeline };
    }

    cells = cellsFor(panelMetrics());
    if (content.compactStrip) {
        for (Cell& cell : cells) {
            if (cell.metric == Metric::Fps) {
                cell.label =
                    tr("Rx/De/Rd", "Received, decoded and rendered frame rates, in that order");
                cell.shortLabel = cell.label;
                if (m_Snapshot.valid) {
                    cell.value = QStringLiteral("%1/%2/%3")
                                     .arg(number(m_Snapshot.receivedFps, 0),
                                          number(m_Snapshot.decodedFps, 0),
                                          number(m_Snapshot.renderedFps, 0));
                }
                break;
            }
        }
    }
    for (const Cell& cell : cells) {
        content.cells.append({ cell.metric, cell.label, cell.shortLabel, cell.value });
    }

    if (panelHas(Metric::FrameTimeGraph)) {
        content.graph = true;
        content.graphTitle = tr("Frame time");
        const FrameTimeSummary& summary = m_Snapshot.frameTimes;
        if (summary.samples > 0) {
            content.graphStats = tr("avg %1 · p99 %2 ms")
                                     .arg(number(summary.averageMs, 1), number(summary.p99Ms, 1));
        }
    }

    if (panelHas(Metric::LatencyBar)) {
        const LatencyBreakdown& latency = m_Snapshot.latency;
        content.latencyBar = true;
        content.latencyTitle = tr("Estimated latency");
        content.latencyValue =
            m_Snapshot.valid ? QStringLiteral("~") + milliseconds(latency.totalMs()) : dash();
        if (latency.hasHost) {
            content.legend.append(tr("Host") + QStringLiteral(" ") + number(latency.hostMs, 1));
        }
        if (latency.hasNetwork) {
            content.legend.append(tr("Network") + QStringLiteral(" ") +
                                  number(latency.networkMs, 1));
        }
        content.legend.append(tr("Decode") + QStringLiteral(" ") + number(latency.decodeMs, 1));
        if (latency.hasQueue) {
            content.legend.append(tr("Queue") + QStringLiteral(" ") + number(latency.queueMs, 1));
        }
        content.legend.append(tr("Render") + QStringLiteral(" ") + number(latency.renderMs, 1));
    }

    content.emptyText = tr("No metrics selected");
    return content;
}

QSize OverlayStatsWindow::panelBodySize() const
{
    QVector<Cell> cells;
    const PanelContent content = panelContent(cells);
    const FontTextMeasurer measurer(overlayFonts());
    const QSize minimum = minimumPanelSize(content, measurer);
    QSize maximum(MaxPanelWidth, MaxPanelHeight);
    if (m_MaximumBodySize.isValid()) {
        maximum = maximum.boundedTo(m_MaximumBodySize);
    }

    if (m_BodySize.isValid()) {
        return clampPanelSize(m_BodySize, minimum, maximum);
    }

    // Default width, as tall as the content
    const int width = std::clamp(defaultPanelWidth(m_Preset), minimum.width(),
                                 std::max(minimum.width(), maximum.width()));
    return layoutPanel(content, QSize(width, 0), measurer).size;
}

void OverlayStatsWindow::paintPanel(QPainter* painter, const PanelLayout& layout,
                                    const PanelContent& content, const QVector<Cell>& cells) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const QFontMetrics valueMetrics(m_ValueFont);

    // Header: big frame rate and detail lines
    if (layout.fpsValueRect.isValid()) {
        const Cell fps = cellFor(Metric::Fps);
        painter->setFont(m_BigFont);
        painter->setPen(gradeColor(fps.grade));
        painter->drawText(layout.fpsValueRect, Qt::AlignLeft | Qt::AlignVCenter, fps.value);
        painter->setFont(m_LabelFont);
        painter->setPen(kTextDim);
        painter->drawText(layout.fpsLabelRect, Qt::AlignLeft | Qt::AlignBottom, fps.label);
        m_HitRegions.append(
            { Metric::Fps, layout.fpsValueRect.united(layout.fpsLabelRect), fps.value });

        // The lines are 1% low, the stream, and the decoder (no tooltip)
        const Cell details[] = { cellFor(Metric::OnePercentLow), cellFor(Metric::Resolution) };
        const QColor colors[] = { gradeColor(details[0].grade), kText, kTextFaint };
        for (int i = 0; i < layout.headerLineRects.size() && i < content.headerLines.size(); i++) {
            const QRect& rect = layout.headerLineRects[i];
            painter->setPen(colors[std::min(i, 2)]);
            painter->drawText(
                rect, Qt::AlignLeft | Qt::AlignVCenter,
                labelMetrics.elidedText(content.headerLines[i], Qt::ElideRight, rect.width()));
            if (i < int(std::size(details))) {
                m_HitRegions.append({ details[i].metric, rect, details[i].value });
            }
        }
    }

    // Grid
    for (const PlacedCell& placed : layout.cells) {
        painter->setFont(m_LabelFont);
        painter->setPen(kTextFaint);
        if (placed.index < 0) {
            painter->drawText(placed.labelRect, Qt::AlignLeft | Qt::AlignVCenter, layout.moreText);
            continue;
        }

        const Cell& cell = cells[placed.index];
        const QString& label = placed.shortLabel ? cell.shortLabel : cell.label;
        painter->drawText(placed.labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                          labelMetrics.elidedText(label, Qt::ElideRight, placed.labelRect.width()));

        painter->setFont(m_ValueFont);
        painter->setPen(gradeColor(cell.grade));
        const Qt::Alignment valueAlignment = layout.style == CellStyle::Inline
                                                 ? Qt::AlignRight | Qt::AlignVCenter
                                                 : Qt::AlignLeft | Qt::AlignVCenter;
        painter->drawText(
            placed.valueRect, valueAlignment,
            valueMetrics.elidedText(cell.value, Qt::ElideRight, placed.valueRect.width()));
        m_HitRegions.append({ cell.metric, placed.rect, cell.value });
    }

    if (layout.graphShown) {
        painter->setFont(m_LabelFont);
        painter->setPen(kTextFaint);
        painter->drawText(layout.graphTitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          content.graphTitle);
        const bool statsOnOwnLine = layout.graphStatsRect.top() != layout.graphTitleRect.top();
        painter->drawText(layout.graphStatsRect,
                          (statsOnOwnLine ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                          labelMetrics.elidedText(content.graphStats, Qt::ElideRight,
                                                  layout.graphStatsRect.width()));
        paintFrameTimeGraph(painter, layout.graphRect);
        m_HitRegions.append({ Metric::FrameTimeGraph,
                              layout.graphTitleRect.united(layout.graphRect), content.graphStats });
    }

    if (layout.latencyShown) {
        paintLatencyBar(painter, layout, content);
        m_HitRegions.append(
            { Metric::LatencyBar,
              layout.latencyTitleRect.united(layout.barRect)
                  .united(layout.legendTextRects.isEmpty() ? QRect()
                                                           : layout.legendTextRects.last()),
              content.latencyValue });
    }

    if (layout.emptyRect.isValid()) {
        painter->setFont(m_LabelFont);
        painter->setPen(kTextDim);
        painter->drawText(layout.emptyRect, Qt::AlignLeft | Qt::AlignVCenter, content.emptyText);
    }
}

void OverlayStatsWindow::paintFrameTimeGraph(QPainter* painter, const QRect& area) const
{
    const FrameTimeSummary& summary = m_Snapshot.frameTimes;
    painter->fillRect(area, kSurface2);

    const std::vector<float>& samples = m_Snapshot.frameTimeHistoryMs;
    const double targetMs = m_Snapshot.targetFps > 0
                                ? 1000.0 / m_Snapshot.targetFps
                                : (summary.averageMs > 0 ? summary.averageMs : 16.7);
    double maxSample = 0;
    for (float sample : samples) {
        maxSample = std::max(maxSample, double(sample));
    }
    // Keep the target line in view without letting one hitch flatten the rest
    const double yMax = std::max(targetMs * 2, std::min(maxSample, targetMs * 4));

    if (!samples.empty()) {
        const double barWidth = double(area.width()) / samples.size();
        for (size_t i = 0; i < samples.size(); i++) {
            const double value = std::min(double(samples[i]), yMax);
            const int barHeight = std::max(1, qRound(value / yMax * area.height()));
            const int x0 = area.left() + qRound(i * barWidth);
            const int x1 = area.left() + qRound((i + 1) * barWidth);
            const Grade grade = gradeFrameTime(samples[i], m_Snapshot.targetFps, thresholds());
            QColor color =
                grade == Grade::Good || grade == Grade::Neutral ? kAccent : gradeColor(grade);
            if (grade == Grade::Good || grade == Grade::Neutral) {
                color.setAlpha(150);
            }
            painter->fillRect(
                QRect(x0, area.bottom() + 1 - barHeight, std::max(1, x1 - x0), barHeight), color);
        }
    }

    // Target frame time
    const int targetY = area.bottom() - qRound(targetMs / yMax * area.height());
    QPen targetPen(kTextDim, 1, Qt::DashLine);
    painter->setPen(targetPen);
    painter->drawLine(area.left(), targetY, area.right(), targetY);
}

void OverlayStatsWindow::paintLatencyBar(QPainter* painter, const PanelLayout& layout,
                                         const PanelContent& content) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const LatencyBreakdown& latency = m_Snapshot.latency;

    // Same order as the legend built in panelContent()
    struct Stage
    {
        double ms;
        QColor color;
    };
    QVector<Stage> stages;
    if (latency.hasHost) {
        stages.append({ latency.hostMs, kStageHost });
    }
    if (latency.hasNetwork) {
        stages.append({ latency.networkMs, kAccent });
    }
    stages.append({ latency.decodeMs, kAcid });
    if (latency.hasQueue) {
        stages.append({ latency.queueMs, kWarn });
    }
    stages.append({ latency.renderMs, kStageRender });

    const Grade totalGrade =
        m_Snapshot.valid
            ? gradeAbove(latency.totalMs(), thresholds().latencyWarnMs, thresholds().latencyBadMs)
            : Grade::Neutral;
    painter->setFont(m_LabelFont);
    painter->setPen(kTextFaint);
    painter->drawText(layout.latencyTitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      content.latencyTitle);
    painter->setPen(gradeColor(totalGrade));
    const bool valueOnOwnLine = layout.latencyValueRect.top() != layout.latencyTitleRect.top();
    painter->drawText(layout.latencyValueRect,
                      (valueOnOwnLine ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                      content.latencyValue);

    const QRect& bar = layout.barRect;
    painter->fillRect(bar, kSurface2);
    const double total = latency.totalMs();
    if (total > 0) {
        double offset = 0;
        for (const Stage& stage : stages) {
            const int x0 = bar.left() + qRound(offset / total * bar.width());
            offset += stage.ms;
            const int x1 = bar.left() + qRound(offset / total * bar.width());
            if (x1 > x0) {
                painter->fillRect(QRect(x0, bar.top(), x1 - x0, bar.height()), stage.color);
            }
        }
    }

    for (int i = 0; i < layout.legendTextRects.size() && i < stages.size(); i++) {
        painter->fillRect(layout.legendSwatchRects[i], stages[i].color);
        painter->setPen(kTextDim);
        const QRect& rect = layout.legendTextRects[i];
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                          labelMetrics.elidedText(content.legend[i], Qt::ElideRight, rect.width()));
    }
}

QString OverlayStatsWindow::editHintText() const
{
    return isResizable() ? tr("Drag to move · Drag the edges to resize · Wheel to zoom · "
                              "Ctrl+wheel to adjust transparency · Esc when done")
                         : tr("Drag to move · Wheel to zoom · Ctrl+wheel to adjust transparency · "
                              "Esc when done");
}

int OverlayStatsWindow::editHintHeight(int width) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const int textWidth = std::max(width - kAccentBar - kPad * 2, 1);
    const QRect text = labelMetrics.boundingRect(QRect(0, 0, textWidth, 10000),
                                                 Qt::TextWordWrap | Qt::AlignLeft, editHintText());
    return text.height() + kPad;
}

void OverlayStatsWindow::paintEditHint(QPainter* painter, int y, int width) const
{
    const int height = editHintHeight(width);
    painter->fillRect(QRect(kAccentBar, y, width - kAccentBar, height), kSurface2);
    painter->setFont(m_LabelFont);
    painter->setPen(kAccent);
    painter->drawText(QRect(kAccentBar + kPad, y, width - kAccentBar - kPad * 2, height),
                      Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, editHintText());
}

void OverlayStatsWindow::paintResizeGrip(QPainter* painter, const QSize& body) const
{
    // Three short diagonals in the bottom-right corner
    painter->setPen(QPen(kAccent, 1));
    const int right = body.width() - 3;
    const int bottom = body.height() - 3;
    for (int i = 1; i <= 3; i++) {
        painter->drawLine(right - i * 3, bottom, right, bottom - i * 3);
    }
}

QSize OverlayStatsWindow::layoutBody(QPainter* painter) const
{
    QSize content;
    switch (m_Preset) {
    case Preset::Minimal:
        content = layoutMinimal(painter);
        break;
    case Preset::Compact:
    case Preset::Detailed:
    case Preset::Custom: {
        content = panelBodySize();
        if (painter != nullptr) {
            QVector<Cell> cells;
            const PanelContent panel = panelContent(cells);
            const PanelLayout layout =
                layoutPanel(panel, content, FontTextMeasurer(overlayFonts()));
            painter->save();
            painter->setClipRect(QRect(QPoint(), content));
            paintPanel(painter, layout, panel, cells);
            painter->restore();
        }
        break;
    }
    case Preset::Off:
    default:
        content = layoutCompact(painter);
        break;
    }

    if (!m_Editing) {
        return content;
    }

    // Auto-sized presets widen to the one-line hint; resizable panels wrap it
    int width = content.width();
    if (!isResizable()) {
        const QFontMetrics labelMetrics(m_LabelFont);
        width = std::max(width, kAccentBar + kPad * 2 +
                                    labelMetrics.horizontalAdvance(editHintText()) + 1);
    }
    if (painter != nullptr) {
        if (isResizable()) {
            paintResizeGrip(painter, content);
        }
        paintEditHint(painter, content.height(), width);
    }
    return QSize(width, content.height() + editHintHeight(width));
}

void OverlayStatsWindow::paintOverlay(QPainter& painter)
{
    m_HitRegions.clear();

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRect(QPoint(), size()), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Square, hard-edged shapes like the rest of the theme; only text is
    // antialiased
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(scale(), scale());

    const QSize bodySize = layoutBody(nullptr);
    const QRect body(QPoint(), bodySize);

    // Align the body inside a window that may be larger than it
    const double spareWidth = width() / scale() - (bodySize.width() + kShadowOffset);
    const double spareHeight = height() / scale() - (bodySize.height() + kShadowOffset);
    double dx = 0;
    double dy = 0;
    if (m_BodyAlignment.testFlag(Qt::AlignRight)) {
        dx = spareWidth;
    } else if (m_BodyAlignment.testFlag(Qt::AlignHCenter)) {
        dx = spareWidth / 2;
    }
    if (m_BodyAlignment.testFlag(Qt::AlignBottom)) {
        dy = spareHeight;
    } else if (m_BodyAlignment.testFlag(Qt::AlignVCenter)) {
        dy = spareHeight / 2;
    }
    m_BodyOffset = QPointF(std::floor(std::max(dx, 0.0)), std::floor(std::max(dy, 0.0)));
    painter.translate(m_BodyOffset);
    const qreal opacity = m_OpacityPercent / 100.0;

    QColor shadow = kShadow;
    shadow.setAlphaF(shadow.alphaF() * opacity);
    painter.fillRect(body.translated(kShadowOffset, kShadowOffset), shadow);

    QColor surface = kSurface;
    surface.setAlphaF(opacity);
    painter.fillRect(body, surface);

    painter.setPen(QPen(m_Editing ? kAccent : kLine, 1, m_Editing ? Qt::DashLine : Qt::SolidLine));
    painter.drawRect(body.adjusted(0, 0, -1, -1));
    painter.fillRect(QRect(0, 0, kAccentBar, body.height()), kAccent);

    layoutBody(&painter);
}

std::optional<OverlayStatsWindow::TooltipInfo>
OverlayStatsWindow::tooltipAt(const QPoint& windowPoint) const
{
    const QPointF bodyPoint = QPointF(windowPoint) / scale() - m_BodyOffset;
    const QPoint point = bodyPoint.toPoint();
    for (const HitRegion& region : m_HitRegions) {
        if (region.rect.contains(point)) {
            return TooltipInfo{ region.metric, region.value };
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Edit mode input
// ---------------------------------------------------------------------------

QPoint OverlayStatsWindow::eventPosition(QMouseEvent* event) const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint local = event->position().toPoint();
    const QPoint global = event->globalPosition().toPoint();
#else
    const QPoint local = event->pos();
    const QPoint global = event->globalPos();
#endif
    // Native Wayland children live in parent-local coordinates
    return OverlayWindowPolicy::usesParentCoordinates(m_WindowMode) ? position() + local : global;
}

QPoint OverlayStatsWindow::localPosition(QMouseEvent* event) const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

int OverlayStatsWindow::edgesAt(const QPoint& local) const
{
    if (!isInteractive() || !isResizable()) {
        return 0;
    }

    // The body, without the shadow and the edit hint below it
    const QSize body = panelBodySize();
    const QRectF rect(m_BodyOffset * scale(), QSizeF(body) * scale());
    const QRectF band = rect.adjusted(-kResizeMargin, -kResizeMargin, kResizeMargin, kResizeMargin);
    if (!band.contains(local)) {
        return 0;
    }

    int edges = 0;
    if (local.x() < rect.left() + kResizeMargin) {
        edges |= EdgeLeft;
    } else if (local.x() > rect.right() - kResizeMargin) {
        edges |= EdgeRight;
    }
    if (local.y() < rect.top() + kResizeMargin) {
        edges |= EdgeTop;
    } else if (local.y() > rect.bottom() - kResizeMargin &&
               local.y() <= rect.bottom() + kResizeMargin) {
        edges |= EdgeBottom;
    }
    return edges;
}

void OverlayStatsWindow::updateEdgeCursor(int edges)
{
    if (!isInteractive()) {
        unsetCursor();
        return;
    }

    const bool horizontal = edges & (EdgeLeft | EdgeRight);
    const bool vertical = edges & (EdgeTop | EdgeBottom);
    Qt::CursorShape shape = Qt::SizeAllCursor;
    if (horizontal && vertical) {
        // Falling diagonal for the top-left and bottom-right corners
        const bool falling = bool(edges & EdgeLeft) == bool(edges & EdgeTop);
        shape = falling ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    } else if (horizontal) {
        shape = Qt::SizeHorCursor;
    } else if (vertical) {
        shape = Qt::SizeVerCursor;
    }
    setCursor(QCursor(shape));
}

void OverlayStatsWindow::resizeTo(const QPoint& pointer)
{
    const QPointF delta = QPointF(pointer - m_PressPosition) / scale();
    QSize body = m_PressBodySize;
    if (m_ResizeEdges & EdgeRight) {
        body.rwidth() += qRound(delta.x());
    } else if (m_ResizeEdges & EdgeLeft) {
        body.rwidth() -= qRound(delta.x());
    }
    if (m_ResizeEdges & EdgeBottom) {
        body.rheight() += qRound(delta.y());
    } else if (m_ResizeEdges & EdgeTop) {
        body.rheight() -= qRound(delta.y());
    }

    m_BodySize = body;
    m_BodySize = panelBodySize(); // clamped

    // The dragged edges move; the opposite ones stay put
    const QSize windowSize = preferredSize();
    QPoint topLeft = m_PressGeometry.topLeft();
    if (m_ResizeEdges & EdgeLeft) {
        topLeft.setX(m_PressGeometry.x() + m_PressGeometry.width() - windowSize.width());
    }
    if (m_ResizeEdges & EdgeTop) {
        topLeft.setY(m_PressGeometry.y() + m_PressGeometry.height() - windowSize.height());
    }
    setGeometry(QRect(topLeft, windowSize));
    repaintNow();
}

void OverlayStatsWindow::mousePressEvent(QMouseEvent* event)
{
    if (!isInteractive()) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_PressPosition = eventPosition(event);
        m_PressWindowPos = position();
        m_ResizeEdges = edgesAt(localPosition(event));
        if (m_ResizeEdges != 0) {
            // Start from exactly the painted size, without the slack the
            // controller may have left around the body
            m_PressBodySize = panelBodySize();
            m_BodyAlignment = Qt::AlignLeft | Qt::AlignTop;
            m_PressGeometry =
                QRect(position() + (m_BodyOffset * scale()).toPoint(), preferredSize());
        } else {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            // Lets the window manager move it, which also works on Wayland,
            // where a window cannot place itself
            if (m_Standalone && startSystemMove()) {
                event->accept();
                return;
            }
#endif
            m_Dragging = true;
        }
    } else if (event->button() == Qt::RightButton && m_Editing && m_EndEditingCallback) {
        m_EndEditingCallback();
    }
    event->accept();
}

void OverlayStatsWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_Standalone) {
        m_HoverPosition = localPosition(event);
    }

    if (m_ResizeEdges != 0) {
        resizeTo(eventPosition(event));
        event->accept();
        return;
    }

    if (!m_Dragging) {
        if (isInteractive()) {
            updateEdgeCursor(edgesAt(localPosition(event)));
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }

    setPosition(m_PressWindowPos + (eventPosition(event) - m_PressPosition));
    event->accept();
}

void OverlayStatsWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || (!m_Dragging && m_ResizeEdges == 0)) {
        event->ignore();
        return;
    }

    if (m_ResizeEdges != 0) {
        m_ResizeEdges = 0;
        if (m_ResizeFinishedCallback) {
            m_ResizeFinishedCallback(geometry().center(), m_BodySize);
        }
    } else {
        m_Dragging = false;
        if (m_DragFinishedCallback) {
            m_DragFinishedCallback(geometry().center());
        }
    }
    event->accept();
}

void OverlayStatsWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_Editing && m_EndEditingCallback) {
        m_EndEditingCallback();
        event->accept();
        return;
    }
    event->ignore();
}

void OverlayStatsWindow::wheelEvent(QWheelEvent* event)
{
    if (!isInteractive() || !m_WheelCallback) {
        event->ignore();
        return;
    }

    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
        m_WheelCallback(steps, event->modifiers().testFlag(Qt::ControlModifier));
    }
    event->accept();
}

void OverlayStatsWindow::keyPressEvent(QKeyEvent* event)
{
    if (m_Editing && event->key() == Qt::Key_Escape && m_EndEditingCallback) {
        m_EndEditingCallback();
        event->accept();
        return;
    }
    event->ignore();
}

void OverlayStatsWindow::moveEvent(QMoveEvent* event)
{
    OverlayRasterWindow::moveEvent(event);
    if (m_MovedCallback) {
        m_MovedCallback(position());
    }
}

bool OverlayStatsWindow::event(QEvent* event)
{
    if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
        m_HoverPosition.reset();
    }
    return OverlayRasterWindow::event(event);
}
