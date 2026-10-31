#include "overlaystatswindow.h"
#include "overlaystatsfonts.h"

#include <QCoreApplication>
#include <QCache>
#include <QCursor>
#include <QDataStream>
#include <QFontMetrics>
#include <QIODevice>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <QtMath>

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
const QColor kGoodText(0x7C, 0xF5, 0xE5);
// Latency stages, chosen to stay apart from the grade colors
const QColor kStageHost(0x8A, 0x9B, 0xFF);
const QColor kStageRender(0xD6, 0x8C, 0xFF);

const int kShadowOffset = 4;
const int kAccentBar = PanelSpacing::AccentBar;
const int kPad = PanelSpacing::Pad;
const int kCellPadX = PanelSpacing::CellPadX;
const int kLabelGap = PanelSpacing::LabelGap;
// Width of the band along the edges that resizes in edit mode, in window pixels
const int kResizeMargin = 12;

void drawOverlayText(QPainter* painter, const QRect& rect, int flags, const QString& text)
{
    if (!rect.isEmpty() && !text.isEmpty()) {
        // Cache masks, not colored text, so opacity/grade changes leave glyphs
        // fully opaque. Take the DPR from the actual paint device (also Wayland).
        static QCache<QByteArray, QImage> shadows(4 * 1024 * 1024);
        const qreal dpr = painter->device()->devicePixelRatioF();
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << painter->font() << rect.size() << flags << text << dpr
               << int(painter->layoutDirection());
        QImage shadow;
        if (const QImage* cached = shadows.object(key)) {
            shadow = *cached;
        } else {
            constexpr int padding = 4;
            shadow = QImage(QSize(qCeil((rect.width() + padding * 2) * dpr),
                                  qCeil((rect.height() + padding * 2) * dpr)),
                            QImage::Format_ARGB32_Premultiplied);
            shadow.setDevicePixelRatio(dpr);
            shadow.fill(Qt::transparent);
            {
                QPainter mask(&shadow);
                mask.setRenderHint(QPainter::TextAntialiasing);
                mask.setFont(painter->font());
                mask.setLayoutDirection(painter->layoutDirection());
                mask.setPen(QColor(0, 0, 0, 153));
                mask.drawText(QRect(QPoint(padding, padding), rect.size()), flags, text);
            }
            QImage horizontal(shadow.size(), shadow.format());
            const int radius = qCeil(3 * dpr);
            const int divisor = (radius + 1) * (radius + 1);
            for (int pass = 0; pass < 2; ++pass) {
                const QImage& source = pass == 0 ? shadow : horizontal;
                QImage& destination = pass == 0 ? horizontal : shadow;
                for (int y = 0; y < shadow.height(); ++y) {
                    auto* row = reinterpret_cast<QRgb*>(destination.scanLine(y));
                    for (int x = 0; x < shadow.width(); ++x) {
                        int alpha = 0;
                        for (int offset = -radius; offset <= radius; ++offset) {
                            const int sx = pass == 0 ? x + offset : x;
                            const int sy = pass == 0 ? y : y + offset;
                            if (sx >= 0 && sx < source.width() && sy >= 0 && sy < source.height()) {
                                const auto* pixels =
                                    reinterpret_cast<const QRgb*>(source.constScanLine(sy));
                                alpha += qAlpha(pixels[sx]) * (radius + 1 - std::abs(offset));
                            }
                        }
                        row[x] = qRgba(0, 0, 0, alpha / divisor);
                    }
                }
            }
            shadows.insert(key, new QImage(shadow), int(shadow.sizeInBytes()));
        }
        painter->drawImage(rect.topLeft() + QPoint(-4, -3), shadow);
    }
    painter->drawText(rect, flags, text);
}

QColor gradeColor(Grade grade)
{
    switch (grade) {
    case Grade::Good:
        return kGoodText;
    case Grade::Warn:
        return kWarn;
    case Grade::Bad:
        return kDanger;
    case Grade::Neutral:
    default:
        return kText;
    }
}

const Thresholds& thresholds()
{
    static const Thresholds defaults;
    return defaults;
}

} // namespace

QString OverlayStatsWindow::dash()
{
    return QStringLiteral("–");
}

QString OverlayStatsWindow::number(double value, int decimals)
{
    if (!std::isfinite(value)) {
        return dash();
    }
    return QString::number(value, 'f', decimals);
}

QString OverlayStatsWindow::milliseconds(double value, int decimals)
{
    return number(value, decimals) + QStringLiteral(" ms");
}

OverlayStatsWindow::OverlayStatsWindow(QWindow* parent, OverlayWindowMode windowMode)
    : OverlayRasterWindow(parent, windowMode), m_WindowMode(windowMode)
{
    Q_ASSERT(m_WindowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(m_WindowMode, true));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setScalePercent(m_ScalePercent);

    m_Metrics = defaultCustomMetrics();
}

void OverlayStatsWindow::setStats(const StatsSnapshot& snapshot, const Extras& extras)
{
    m_Snapshot = snapshot;
    m_Extras = extras;
}

void OverlayStatsWindow::setPreset(Preset preset)
{
    if (m_Preset != preset) {
        m_StripValueWidths.clear();
    }
    m_Preset = preset;
}

void OverlayStatsWindow::setMetrics(const QVector<Metric>& metrics)
{
    m_Metrics = metrics;
}

void OverlayStatsWindow::setScalePercent(int percent)
{
    if (m_ScalePercent != clampScalePercent(percent)) {
        m_StripValueWidths.clear();
    }
    m_ScalePercent = clampScalePercent(percent);
    const OverlayFonts fonts = overlayFonts(m_ScalePercent);
    m_LabelFont = fonts.label;
    m_ValueFont = fonts.value;
    m_BigFont = fonts.big;
}

QPoint OverlayStatsWindow::bodyPosition() const
{
    return position() + (m_BodyOffset * scale()).toPoint();
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
        setFlags(OverlayWindowPolicy::flags(m_WindowMode, !isInteractive()));
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
                                      : OverlayWindowPolicy::flags(windowMode, !isInteractive());
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
    m_PressedControl = -1;
    m_HoverControl = -1;
    if (!editing) {
        unsetCursor();
    }
    updateInputFlags();
}

void OverlayStatsWindow::setEditOnClick(bool enabled)
{
    m_EditOnClick = enabled;
    updateInputFlags();
}

void OverlayStatsWindow::updateInputFlags()
{
    if (m_Standalone) {
        // Always takes input, so the flags stay as they are
        return;
    }
    const auto flags = OverlayWindowPolicy::flags(m_WindowMode, !isInteractive());
    if (this->flags() == flags) {
        return;
    }

    // Input transparency is a window flag, which some platforms only apply
    // when the native window is recreated, so hide around the change.
    const bool wasVisible = isVisible();
    if (wasVisible) {
        hide();
    }
    setFlags(flags);
    if (wasVisible) {
        show();
        raise();
    }
}

QSize OverlayStatsWindow::preferredSize() const
{
    const QSize body = layoutBody(nullptr);
    return QSize((body.width() * m_ScalePercent + 99) / 100,
                 (body.height() * m_ScalePercent + 99) / 100);
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
        cell.value =
            s.valid && s.frameTimes.samples > 0 ? number(s.frameTimes.onePercentLowFps, 0) : dash();
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
    return m_Preset == Preset::Detailed ||
           (m_Preset == Preset::Custom && m_Metrics.contains(metric));
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
            drawOverlayText(painter, QRect(x, 0, valueWidth, height),
                            Qt::AlignLeft | Qt::AlignVCenter, cell.value);
            painter->setFont(m_LabelFont);
            painter->setPen(kTextDim);
            drawOverlayText(painter, QRect(x + valueWidth + kLabelGap, 0, labelWidth, height),
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
            drawOverlayText(painter, QRect(x + kCellPadX, kPad, width, labelMetrics.height()),
                            Qt::AlignLeft | Qt::AlignVCenter, cell.label);
            painter->setFont(m_ValueFont);
            painter->setPen(gradeColor(cell.grade));
            drawOverlayText(painter,
                            QRect(x + kCellPadX, kPad + labelMetrics.height() + kLabelGap, width,
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
    const FontTextMeasurer measurer(overlayFonts(m_ScalePercent));
    const QSize minimum = minimumPanelSize(content, measurer);
    QSize maximum(MaxPanelWidth, MaxPanelHeight);
    if (m_MaximumBodySize.isValid()) {
        maximum = maximum.boundedTo(m_MaximumBodySize);
    }

    QSize size;
    if (m_BodySize.isValid()) {
        size = clampPanelSize(m_BodySize, minimum, maximum);
    } else {
        // Default width, as tall as the content
        const int width = std::clamp(defaultPanelWidth(m_Preset), minimum.width(),
                                     std::max(minimum.width(), maximum.width()));
        size = layoutPanel(content, QSize(width, 0), measurer).size;
    }

    if (m_Preset == Preset::Compact) {
        // A saved short height must not discard metrics after the bar wraps.
        size = layoutPanel(content, QSize(size.width(), 0), measurer).size;
        size.setHeight(std::min(size.height(), maximum.height()));
    }

    // The move-mode controls and hint sit below the body and must stay inside
    // the same bounds, however narrow the panel makes them wrap
    if (m_Editing && m_MaximumBodySize.isValid()) {
        auto footerHeight = [&] {
            return layoutEditControls(nullptr, 0, size.width()) + editHintHeight(size.width());
        };
        int footer = footerHeight();
        // A very narrow saved panel can make the toolbar taller than the
        // stream. Widen it temporarily rather than put Done offscreen.
        while (minimum.height() + footer > maximum.height() && size.width() < maximum.width()) {
            size.setWidth(std::min(size.width() + kPad * 4, maximum.width()));
            footer = footerHeight();
        }
        if (m_Preset == Preset::Compact) {
            size = layoutPanel(content, QSize(size.width(), 0), measurer).size;
        }
        const int limit = std::max(minimum.height(), m_MaximumBodySize.height() - footer);
        size.setHeight(std::min(size.height(), limit));
    }
    return size;
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
        drawOverlayText(painter, layout.fpsValueRect, Qt::AlignLeft | Qt::AlignVCenter, fps.value);
        painter->setFont(m_LabelFont);
        painter->setPen(kTextDim);
        drawOverlayText(painter, layout.fpsLabelRect, Qt::AlignLeft | Qt::AlignBottom, fps.label);
        m_HitRegions.append(
            { Metric::Fps, layout.fpsValueRect.united(layout.fpsLabelRect), fps.value });

        // The lines are 1% low, the stream, and the decoder (no tooltip)
        const Cell details[] = { cellFor(Metric::OnePercentLow), cellFor(Metric::Resolution) };
        const QColor colors[] = { gradeColor(details[0].grade), kText, kTextFaint };
        for (int i = 0; i < layout.headerLineRects.size() && i < content.headerLines.size(); i++) {
            const QRect& rect = layout.headerLineRects[i];
            painter->setPen(colors[std::min(i, 2)]);
            drawOverlayText(
                painter, rect, Qt::AlignLeft | Qt::AlignVCenter,
                labelMetrics.elidedText(content.headerLines[i], Qt::ElideRight, rect.width()));
            if (i < int(std::size(details))) {
                m_HitRegions.append({ details[i].metric, rect, details[i].value });
            }
        }
    }

    // Grid
    for (const PlacedCell& placed : layout.cells) {
        QRect chip;
        if (m_Preset == Preset::Compact) {
            // Use stable layout widths, not live glyph widths, to avoid a
            // wobbling background as the values change.
            chip = placed.rect.adjusted(-3, -2, 3, 2);
            if (placed.rect.top() == kPad) {
                chip.setTop(0);
            }
            QColor surface = kSurface;
            surface.setAlphaF(m_OpacityPercent / 100.0);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(surface);
            painter->drawRoundedRect(chip, 3, 3);
            painter->restore();
        }
        painter->setFont(m_LabelFont);
        painter->setPen(kText);
        if (placed.index < 0) {
            drawOverlayText(painter, placed.labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                            layout.moreText);
            continue;
        }

        const Cell& cell = cells[placed.index];
        const QString& label = placed.shortLabel ? cell.shortLabel : cell.label;
        drawOverlayText(painter, placed.labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                        labelMetrics.elidedText(label, Qt::ElideRight, placed.labelRect.width()));

        painter->setFont(m_ValueFont);
        painter->setPen(gradeColor(cell.grade));
        const Qt::Alignment valueAlignment = layout.style == CellStyle::Inline
                                                 ? Qt::AlignRight | Qt::AlignVCenter
                                                 : Qt::AlignLeft | Qt::AlignVCenter;
        drawOverlayText(
            painter, placed.valueRect, valueAlignment,
            valueMetrics.elidedText(cell.value, Qt::ElideRight, placed.valueRect.width()));
        m_HitRegions.append({ cell.metric, chip.isValid() ? chip : placed.rect, cell.value });
    }

    if (layout.graphShown) {
        painter->setFont(m_LabelFont);
        painter->setPen(kTextFaint);
        drawOverlayText(painter, layout.graphTitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                        content.graphTitle);
        const bool statsOnOwnLine = layout.graphStatsRect.top() != layout.graphTitleRect.top();
        drawOverlayText(painter, layout.graphStatsRect,
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
        drawOverlayText(painter, layout.emptyRect, Qt::AlignLeft | Qt::AlignVCenter,
                        content.emptyText);
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
    drawOverlayText(painter, layout.latencyTitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                    content.latencyTitle);
    painter->setPen(gradeColor(totalGrade));
    const bool valueOnOwnLine = layout.latencyValueRect.top() != layout.latencyTitleRect.top();
    drawOverlayText(painter, layout.latencyValueRect,
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
        drawOverlayText(painter, rect, Qt::AlignLeft | Qt::AlignVCenter,
                        labelMetrics.elidedText(content.legend[i], Qt::ElideRight, rect.width()));
    }
}

QString OverlayStatsWindow::editHintText() const
{
    const QString hint =
        isResizable() ? tr("Drag to move · Drag the edges to resize · Wheel to zoom · "
                           "Ctrl+wheel to adjust transparency · Esc when done")
                      : tr("Drag to move · Wheel to zoom · Ctrl+wheel to adjust transparency · "
                           "Esc when done");
    const QString status = tr("Size: %1% · Opacity: %2%").arg(m_ScalePercent).arg(m_OpacityPercent);
    return status + QLatin1Char('\n') + hint +
           (m_Standalone ? QLatin1Char('\n') + tr("Detached windows may increase latency.")
                         : QString());
}

int OverlayStatsWindow::layoutEditControls(QPainter* painter, int y, int width) const
{
    QString preset;
    switch (m_Preset) {
    case Preset::Off:
        preset = tr("Off");
        break;
    case Preset::Minimal:
        preset = tr("Minimal");
        break;
    case Preset::Compact:
        preset = tr("Compact");
        break;
    case Preset::Detailed:
        preset = tr("Detailed");
        break;
    case Preset::Custom:
        preset = tr("Custom");
        break;
    }
    const std::array<QString, ControlCount> labels = {
        m_Standalone ? tr("Re-attach") : tr("Detach"),
        tr("Details: %1").arg(preset),
        m_Preset == Preset::Custom ? tr("Custom: On") : tr("Custom: Off"),
        tr("Opacity -"),
        tr("Opacity +"),
        tr("Size -"),
        tr("Size +"),
        tr("Reset"),
        tr("Done")
    };
    const QFontMetrics metrics(m_LabelFont);
    const int left = kAccentBar + kPad;
    const int available = std::max(width - left - kPad, 1);
    int x = left;
    int top = y + kPad;
    int rowHeight = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        const int buttonWidth = std::min(available, metrics.horizontalAdvance(labels[i]) + 18);
        const QRect textBounds = metrics.boundingRect(
            QRect(0, 0, std::max(buttonWidth - 16, 1), 10000), Qt::TextWrapAnywhere, labels[i]);
        const int buttonHeight = std::max(qCeil(28 / scale()), textBounds.height() + 12);
        if (x != left && x + buttonWidth > width - kPad) {
            x = left;
            top += rowHeight + kPad;
            rowHeight = 0;
        }
        const QRect rect(x, top, buttonWidth, buttonHeight);
        if (painter != nullptr) {
            m_EditControls[i] = rect;
            const bool enabled = editControlEnabled(int(i));
            const bool highlighted =
                enabled && (m_PressedControl == int(i) || m_HoverControl == int(i));
            painter->fillRect(rect, highlighted ? kLine : kSurface2);
            painter->setPen(highlighted ? kAccent : kTextDim);
            painter->drawRect(rect.adjusted(0, 0, -1, -1));
            painter->setFont(m_LabelFont);
            painter->setPen(enabled ? kText : kTextFaint);
            drawOverlayText(painter, rect.adjusted(8, 6, -8, -6),
                            Qt::AlignCenter | Qt::TextWrapAnywhere, labels[i]);
        }
        x += buttonWidth + kPad;
        rowHeight = std::max(rowHeight, buttonHeight);
    }
    return top + rowHeight + kPad - y;
}

bool OverlayStatsWindow::editControlEnabled(int control) const
{
    switch (control) {
    case OpacityDownControl:
        return m_OpacityPercent > MinOpacityPercent;
    case OpacityUpControl:
        return m_OpacityPercent < MaxOpacityPercent;
    case SizeDownControl:
        return m_ScalePercent > MinScalePercent;
    case SizeUpControl:
        return m_ScalePercent < MaxScalePercent;
    default:
        return control >= 0 && control < ControlCount;
    }
}

int OverlayStatsWindow::editControlAt(const QPoint& local) const
{
    if (m_Editing) {
        const QPoint point = (QPointF(local) / scale() - m_BodyOffset).toPoint();
        for (size_t i = 0; i < m_EditControls.size(); ++i) {
            if (m_EditControls[i].contains(point)) {
                return int(i);
            }
        }
    }
    return -1;
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
    drawOverlayText(painter, QRect(kAccentBar + kPad, y, width - kAccentBar - kPad * 2, height),
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
    case Preset::Compact:
    case Preset::Detailed:
        content = layoutStrip(painter);
        break;
    case Preset::Custom: {
        content = panelBodySize();
        if (painter != nullptr) {
            QVector<Cell> cells;
            const PanelContent panel = panelContent(cells);
            const PanelLayout layout =
                layoutPanel(panel, content, FontTextMeasurer(overlayFonts(m_ScalePercent)));
            painter->save();
            painter->setClipRect(QRect(QPoint(), content));
            paintPanel(painter, layout, panel, cells);
            painter->restore();
        }
        break;
    }
    case Preset::Off:
        content = QSize(320, QFontMetrics(m_LabelFont).height() + kPad * 2);
        if (painter != nullptr && m_Editing) {
            painter->setFont(m_LabelFont);
            painter->setPen(kTextDim);
            drawOverlayText(painter, QRect(QPoint(kPad, 0), content - QSize(kPad * 2, 0)),
                            Qt::AlignLeft | Qt::AlignVCenter, tr("Overlay off"));
        }
        break;
    default:
        content = layoutCompact(painter);
        break;
    }

    if (!m_Editing) {
        return content;
    }

    const int width = content.width();
    const int controlsHeight = layoutEditControls(painter, content.height(), width);
    if (painter != nullptr) {
        if (isResizable()) {
            paintResizeGrip(painter, content);
        }
        paintEditHint(painter, content.height() + controlsHeight, width);
    }
    return QSize(width, content.height() + controlsHeight + editHintHeight(width));
}

void OverlayStatsWindow::paintOverlay(QPainter& painter)
{
    m_HitRegions.clear();
    m_EditControls = {};

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
    const double spareWidth = width() / scale() - bodySize.width();
    const double spareHeight = height() / scale() - bodySize.height();
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

    QColor surface(Qt::black);
    surface.setAlphaF(opacity);
    painter.fillRect(body, surface);
    if (m_Editing) {
        painter.setPen(QPen(kAccent, 1, Qt::DashLine));
        painter.drawRect(body.adjusted(0, 0, -1, -1));
    }

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

QSize OverlayStatsWindow::metricBodySize() const
{
    return m_Preset == Preset::Custom ? panelBodySize() : layoutStrip(nullptr);
}

int OverlayStatsWindow::edgesAt(const QPoint& local) const
{
    if (!isInteractive() || !isResizable()) {
        return 0;
    }

    const QSize body = metricBodySize();
    const QRectF rect(m_BodyOffset * scale(), QSizeF(body) * scale());
    // Keep a central move zone even on the short Compact strip. The whole
    // painted instruction footer belongs to the bottom resize edge.
    const qreal horizontalMargin = std::min(qreal(kResizeMargin), rect.width() / 3);
    const qreal verticalMargin = std::min(qreal(kResizeMargin), rect.height() / 3);
    const qreal bottom =
        m_Editing ? (m_BodyOffset.y() + layoutBody(nullptr).height()) * scale() : rect.bottom();
    const QRectF band = rect.adjusted(-horizontalMargin, -verticalMargin, horizontalMargin,
                                      std::max(qreal(kResizeMargin), bottom - rect.bottom()));
    if (!band.contains(local)) {
        return 0;
    }

    int edges = 0;
    if (local.x() < rect.left() + horizontalMargin) {
        edges |= EdgeLeft;
    } else if (local.x() > rect.right() - horizontalMargin) {
        edges |= EdgeRight;
    }
    if (local.y() < rect.top() + verticalMargin) {
        edges |= EdgeTop;
    } else if (local.y() > rect.bottom() - verticalMargin) {
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

    setBodySize(body);
    m_BodySize = metricBodySize(); // clamped

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
    if (m_EditOnClick && !m_Editing && event->button() == Qt::LeftButton &&
        m_BeginEditingCallback) {
        m_BeginEditingCallback();
        // The toolbar may now occupy the click's original position.
        m_PressedControl = ControlCount;
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_PressedControl = editControlAt(localPosition(event));
        if (m_PressedControl >= 0) {
            m_PressedControlRect = m_EditControls[m_PressedControl];
            repaintNow();
            event->accept();
            return;
        }
        m_PressPosition = eventPosition(event);
        m_PressWindowPos = position();
        m_ResizeEdges = edgesAt(localPosition(event));
        if (m_ResizeEdges != 0) {
            // Start from exactly the painted size, without the slack the
            // controller may have left around the body
            m_PressBodySize = metricBodySize();
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
    const int control = editControlAt(localPosition(event));
    if (control != m_HoverControl) {
        m_HoverControl = control;
        repaintNow();
    }
    if (m_PressedControl >= 0 || (control >= 0 && !isDragging())) {
        setCursor(Qt::PointingHandCursor);
        event->accept();
        return;
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

    QPoint target = m_PressWindowPos + (eventPosition(event) - m_PressPosition);
    if (!m_Standalone && m_MovementBounds.isValid()) {
        target = lightlySnappedPosition(target, size(), m_MovementBounds);
    }
    setPosition(target);
    event->accept();
}

void OverlayStatsWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_PressedControl >= 0) {
        const int pressed = m_PressedControl;
        m_PressedControl = -1;
        event->accept();
        repaintNow();
        if (editControlAt(localPosition(event)) == pressed && editControlEnabled(pressed) &&
            m_EditControls[pressed] == m_PressedControlRect) {
            std::function<void()> callback;
            switch (pressed) {
            case DetachControl:
                callback = m_ToggleDetachedCallback;
                break;
            case PresetControl:
                callback = m_CyclePresetCallback;
                break;
            case CustomControl:
                callback = m_ToggleCustomCallback;
                break;
            case ResetControl:
                callback = m_ResetCallback;
                break;
            case DoneControl:
                callback = m_EndEditingCallback;
                break;
            case OpacityDownControl:
            case OpacityUpControl:
            case SizeDownControl:
            case SizeUpControl:
                if (m_WheelCallback) {
                    m_WheelCallback(pressed == OpacityDownControl || pressed == SizeDownControl ? -1
                                                                                                : 1,
                                    pressed == OpacityDownControl || pressed == OpacityUpControl);
                }
                break;
            }
            if (callback) {
                callback();
            }
        }
        return;
    }
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
    const int control = editControlAt(localPosition(event));
    if (control == PresetControl || (control >= OpacityDownControl && control <= SizeUpControl)) {
        // Every click of a cycle control counts, so rapid clicks keep cycling
        mousePressEvent(event);
        return;
    }
    if (control >= 0) {
        // The first click already acted; a second would silently undo a toggle
        m_PressedControl = ControlCount;
        event->accept();
        return;
    }
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
        m_HoverControl = -1;
        if (event->type() == QEvent::Hide) {
            m_PressedControl = -1;
        }
    }
    return OverlayRasterWindow::event(event);
}
