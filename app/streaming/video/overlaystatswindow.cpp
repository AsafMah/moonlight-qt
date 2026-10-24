#include "overlaystatswindow.h"
#include "uifont.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

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
const int kAccentBar = 3;
const int kPad = 8;
const int kCellPadX = 10;
const int kLabelGap = 3;
const int kPanelWidth = 300;
const int kGraphHeight = 56;
const int kBarHeight = 8;
const int kSectionGap = 8;

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

    // Fixed pixel sizes for the same reason as OverlayToast: point sizes go
    // through the platform font DPI, which differs from Qt's UI scale on Linux
    m_LabelFont.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    m_LabelFont.setPixelSize(10);
    m_LabelFont.setCapitalization(QFont::AllUppercase);
    m_LabelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);

    m_ValueFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    m_ValueFont.setPixelSize(14);
    m_ValueFont.setWeight(QFont::DemiBold);
    m_ValueFont.setStyleHint(QFont::SansSerif);

    m_BigFont = m_ValueFont;
    m_BigFont.setPixelSize(30);
    m_BigFont.setWeight(QFont::Bold);

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

void OverlayStatsWindow::setEditing(bool editing)
{
    if (m_Editing == editing) {
        return;
    }

    m_Editing = editing;
    m_Dragging = false;

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

    if (!s.valid) {
        cell.value = dash();
    }

    switch (metric) {
    case Metric::Resolution:
        cell.label = tr("Stream");
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
        if (s.valid) {
            cell.value = QStringLiteral("~") + milliseconds(s.latency.totalMs(), 0);
            cell.grade = gradeAbove(s.latency.totalMs(), t.latencyWarnMs, t.latencyBadMs);
        }
        break;
    case Metric::Bitrate:
        cell.label = tr("Bitrate");
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
        if (s.valid) {
            cell.value = milliseconds(s.latency.decodeMs);
            cell.grade = gradeAbove(s.latency.decodeMs, t.decodeWarnMs, t.decodeBadMs);
        }
        break;
    case Metric::Queue:
        cell.label = tr("Queue");
        if (s.valid) {
            cell.value = milliseconds(s.latency.queueMs);
        }
        break;
    case Metric::Render:
        cell.label = tr("Render");
        if (s.valid) {
            cell.value = milliseconds(s.latency.renderMs);
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
        if (s.valid) {
            cell.value = number(s.pacerDropPercent, 1) + QStringLiteral("%");
            cell.grade =
                gradeAbove(s.pacerDropPercent, t.pacerDropWarnPercent, t.pacerDropBadPercent);
        }
        break;
    case Metric::DisplayHz:
        cell.label = tr("Display");
        if (m_Extras.displayHz > 0) {
            cell.value = QStringLiteral("%1 Hz").arg(number(m_Extras.displayHz, 0));
            if (s.targetFps > 0) {
                cell.value += QStringLiteral(" / %1 fps").arg(s.targetFps);
                // A display slower than the stream has to drop frames
                cell.grade = m_Extras.displayHz + 0.5 < s.targetFps ? Grade::Warn : Grade::Good;
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
        cells.append(cellFor(metric));
    }
    return cells;
}

QVector<Metric> OverlayStatsWindow::panelMetrics() const
{
    if (m_Preset == Preset::Custom) {
        return m_Metrics;
    }

    QVector<Metric> metrics = { Metric::Rtt,  Metric::Jitter,     Metric::Bitrate,
                                Metric::Loss, Metric::PacerDrops, Metric::DisplayHz,
                                Metric::Abr,  Metric::SessionTime };
    // FEC only when the transport reports it
    if (m_Snapshot.videoPackets > 0) {
        metrics.insert(4, Metric::Fec);
    }
    return metrics;
}

bool OverlayStatsWindow::panelHas(Metric metric) const
{
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
        }
        x += width;
    }

    return QSize(x, height);
}

int OverlayStatsWindow::paintPanelHeader(QPainter* painter, int y, int width) const
{
    const QFontMetrics bigMetrics(m_BigFont);
    const QFontMetrics labelMetrics(m_LabelFont);
    const int left = kAccentBar + kPad;
    const int lineHeight = labelMetrics.height() + 2;
    const int height = std::max(bigMetrics.height(), lineHeight * 3);

    if (painter == nullptr) {
        return height;
    }

    const Cell fps = cellFor(Metric::Fps);
    const int fpsWidth = bigMetrics.horizontalAdvance(fps.value);
    painter->setFont(m_BigFont);
    painter->setPen(gradeColor(fps.grade));
    painter->drawText(QRect(left, y, fpsWidth, height), Qt::AlignLeft | Qt::AlignVCenter,
                      fps.value);

    painter->setFont(m_LabelFont);
    painter->setPen(kTextDim);
    const int fpsLabelX = left + fpsWidth + kLabelGap;
    const int fpsLabelWidth = labelMetrics.horizontalAdvance(fps.label);
    painter->drawText(QRect(fpsLabelX, y, fpsLabelWidth, height - 6),
                      Qt::AlignLeft | Qt::AlignBottom, fps.label);

    // Three lines to the right of the big number
    const int textX = fpsLabelX + fpsLabelWidth + kCellPadX;
    const int textWidth = width - textX - kPad;
    const int top = y + (height - lineHeight * 3) / 2;

    const Cell low = cellFor(Metric::OnePercentLow);
    QString lows = low.label + QStringLiteral(" ") + low.value;
    if (m_Snapshot.frameTimes.samples > 0) {
        lows += QStringLiteral("  ") + tr("0.1% low") + QStringLiteral(" ") +
                number(m_Snapshot.frameTimes.pointOnePercentLowFps, 0);
    }
    painter->setPen(gradeColor(low.grade));
    painter->drawText(QRect(textX, top, textWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                      labelMetrics.elidedText(lows, Qt::ElideRight, textWidth));

    painter->setPen(kText);
    painter->drawText(
        QRect(textX, top + lineHeight, textWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
        labelMetrics.elidedText(cellFor(Metric::Resolution).value, Qt::ElideRight, textWidth));

    QString pipeline = m_Snapshot.decoder;
    if (!m_Snapshot.renderer.isEmpty()) {
        pipeline += QStringLiteral(" · ") + m_Snapshot.renderer;
    }
    painter->setPen(kTextFaint);
    painter->drawText(QRect(textX, top + lineHeight * 2, textWidth, lineHeight),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      labelMetrics.elidedText(pipeline, Qt::ElideRight, textWidth));

    return height;
}

int OverlayStatsWindow::paintFrameTimeGraph(QPainter* painter, int y, int width) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const int height = labelMetrics.height() + kLabelGap + kGraphHeight;
    if (painter == nullptr) {
        return height;
    }

    const int left = kAccentBar + kPad;
    const int graphWidth = width - left - kPad;
    const FrameTimeSummary& summary = m_Snapshot.frameTimes;

    painter->setFont(m_LabelFont);
    painter->setPen(kTextFaint);
    painter->drawText(QRect(left, y, graphWidth, labelMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, tr("Frame time"));
    if (summary.samples > 0) {
        painter->drawText(
            QRect(left, y, graphWidth, labelMetrics.height()), Qt::AlignRight | Qt::AlignVCenter,
            tr("avg %1 · p99 %2 ms").arg(number(summary.averageMs, 1), number(summary.p99Ms, 1)));
    }

    const QRect area(left, y + labelMetrics.height() + kLabelGap, graphWidth, kGraphHeight);
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

    return height;
}

int OverlayStatsWindow::paintLatencyBar(QPainter* painter, int y, int width) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const int left = kAccentBar + kPad;
    const int barWidth = width - left - kPad;
    const LatencyBreakdown& latency = m_Snapshot.latency;

    struct Stage
    {
        QString name;
        double ms;
        QColor color;
    };
    QVector<Stage> stages;
    if (latency.hasHost) {
        stages.append({ tr("Host"), latency.hostMs, kStageHost });
    }
    if (latency.hasNetwork) {
        stages.append({ tr("Network"), latency.networkMs, kAccent });
    }
    stages.append({ tr("Decode"), latency.decodeMs, kAcid });
    stages.append({ tr("Queue"), latency.queueMs, kWarn });
    stages.append({ tr("Render"), latency.renderMs, kStageRender });

    // Legend entries flow into as many lines as they need
    const int swatch = 6;
    int legendLines = 1;
    int legendX = 0;
    QVector<QPoint> legendPositions;
    QVector<QString> legendTexts;
    for (const Stage& stage : stages) {
        const QString text = stage.name + QStringLiteral(" ") + number(stage.ms, 1);
        const int entryWidth = swatch + 4 + labelMetrics.horizontalAdvance(text);
        if (legendX > 0 && legendX + entryWidth > barWidth) {
            legendLines++;
            legendX = 0;
        }
        legendPositions.append(QPoint(legendX, legendLines - 1));
        legendTexts.append(text);
        legendX += entryWidth + kCellPadX;
    }

    const int lineHeight = labelMetrics.height();
    const int height = lineHeight + kLabelGap + kBarHeight + kLabelGap + lineHeight * legendLines;
    if (painter == nullptr) {
        return height;
    }

    const Grade totalGrade =
        m_Snapshot.valid
            ? gradeAbove(latency.totalMs(), thresholds().latencyWarnMs, thresholds().latencyBadMs)
            : Grade::Neutral;
    painter->setFont(m_LabelFont);
    painter->setPen(kTextFaint);
    painter->drawText(QRect(left, y, barWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                      tr("Estimated latency"));
    painter->setPen(gradeColor(totalGrade));
    painter->drawText(QRect(left, y, barWidth, lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                      m_Snapshot.valid ? QStringLiteral("~") + milliseconds(latency.totalMs())
                                       : dash());

    const QRect bar(left, y + lineHeight + kLabelGap, barWidth, kBarHeight);
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

    const int legendTop = bar.bottom() + 1 + kLabelGap;
    for (int i = 0; i < stages.size(); i++) {
        const int x = left + legendPositions[i].x();
        const int lineY = legendTop + legendPositions[i].y() * lineHeight;
        painter->fillRect(QRect(x, lineY + (lineHeight - swatch) / 2, swatch, swatch),
                          stages[i].color);
        painter->setPen(kTextDim);
        painter->drawText(QRect(x + swatch + 4, lineY, barWidth, lineHeight),
                          Qt::AlignLeft | Qt::AlignVCenter, legendTexts[i]);
    }

    return height;
}

int OverlayStatsWindow::paintGrid(QPainter* painter, int y, int width,
                                  const QVector<Cell>& cells) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const QFontMetrics valueMetrics(m_ValueFont);
    const int rowHeight = std::max(labelMetrics.height(), valueMetrics.height()) + 2;
    const int rows = (cells.size() + 1) / 2;
    const int height = rows * rowHeight;
    if (painter == nullptr) {
        return height;
    }

    const int left = kAccentBar + kPad;
    const int columnGap = kCellPadX;
    const int columnWidth = (width - left - kPad - columnGap) / 2;

    for (int i = 0; i < cells.size(); i++) {
        const int column = i % 2;
        const int row = i / 2;
        const QRect rect(left + column * (columnWidth + columnGap), y + row * rowHeight,
                         columnWidth, rowHeight);
        const Cell& cell = cells[i];

        painter->setFont(m_LabelFont);
        painter->setPen(kTextFaint);
        const int labelWidth =
            std::min(labelMetrics.horizontalAdvance(cell.label), columnWidth / 2);
        painter->drawText(QRect(rect.left(), rect.top(), labelWidth, rect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          labelMetrics.elidedText(cell.label, Qt::ElideRight, labelWidth));

        painter->setFont(m_ValueFont);
        painter->setPen(gradeColor(cell.grade));
        const int valueWidth = rect.width() - labelWidth - kLabelGap * 2;
        painter->drawText(QRect(rect.right() - valueWidth, rect.top(), valueWidth, rect.height()),
                          Qt::AlignRight | Qt::AlignVCenter,
                          valueMetrics.elidedText(cell.value, Qt::ElideLeft, valueWidth));
    }

    return height;
}

QSize OverlayStatsWindow::layoutPanel(QPainter* painter) const
{
    const int width = kPanelWidth;
    int y = kPad;
    bool empty = true;

    auto section = [&](int height) {
        y += height + kSectionGap;
        empty = false;
    };

    if (m_Preset == Preset::Detailed) {
        section(paintPanelHeader(painter, y, width));
    }

    const QVector<Cell> cells = cellsFor(panelMetrics());
    if (!cells.isEmpty()) {
        section(paintGrid(painter, y, width, cells));
    }

    if (panelHas(Metric::FrameTimeGraph)) {
        section(paintFrameTimeGraph(painter, y, width));
    }
    if (panelHas(Metric::LatencyBar)) {
        section(paintLatencyBar(painter, y, width));
    }

    if (empty) {
        const QFontMetrics labelMetrics(m_LabelFont);
        if (painter != nullptr) {
            painter->setFont(m_LabelFont);
            painter->setPen(kTextDim);
            painter->drawText(
                QRect(kAccentBar + kPad, y, width - kAccentBar - kPad * 2, labelMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter, tr("No metrics selected"));
        }
        y += labelMetrics.height() + kSectionGap;
    }

    // The last section's gap becomes the bottom padding
    return QSize(width, y - kSectionGap + kPad);
}

int OverlayStatsWindow::paintEditHint(QPainter* painter, int y, int width) const
{
    const QFontMetrics labelMetrics(m_LabelFont);
    const int height = labelMetrics.height() + kPad;
    if (painter != nullptr) {
        painter->fillRect(QRect(kAccentBar, y, width - kAccentBar, height), kSurface2);
        painter->setFont(m_LabelFont);
        painter->setPen(kAccent);
        painter->drawText(QRect(kAccentBar + kPad, y, width - kAccentBar - kPad * 2, height),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          tr("Drag to move · Wheel to resize · Esc when done"));
    }
    return height;
}

QSize OverlayStatsWindow::layoutBody(QPainter* painter) const
{
    QSize content;
    switch (m_Preset) {
    case Preset::Minimal:
        content = layoutMinimal(painter);
        break;
    case Preset::Detailed:
    case Preset::Custom:
        content = layoutPanel(painter);
        break;
    case Preset::Compact:
    case Preset::Off:
    default:
        content = layoutCompact(painter);
        break;
    }

    if (!m_Editing) {
        return content;
    }

    const QFontMetrics labelMetrics(m_LabelFont);
    const int hintWidth =
        kAccentBar + kPad * 2 +
        labelMetrics.horizontalAdvance(tr("Drag to move · Wheel to resize · Esc when done"));
    const int width = std::max(content.width(), hintWidth);
    const int hintHeight = paintEditHint(painter, content.height(), width);
    return QSize(width, content.height() + hintHeight);
}

void OverlayStatsWindow::paintOverlay(QPainter& painter)
{
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
    painter.translate(std::floor(std::max(dx, 0.0)), std::floor(std::max(dy, 0.0)));
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

void OverlayStatsWindow::mousePressEvent(QMouseEvent* event)
{
    if (!m_Editing) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_Dragging = true;
        m_PressPosition = eventPosition(event);
        m_PressWindowPos = position();
    } else if (event->button() == Qt::RightButton && m_EndEditingCallback) {
        m_EndEditingCallback();
    }
    event->accept();
}

void OverlayStatsWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_Dragging) {
        event->ignore();
        return;
    }

    setPosition(m_PressWindowPos + (eventPosition(event) - m_PressPosition));
    event->accept();
}

void OverlayStatsWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_Dragging || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    m_Dragging = false;
    if (m_DragFinishedCallback) {
        m_DragFinishedCallback(geometry().center());
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
    if (!m_Editing || !m_WheelCallback) {
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
