#include "overlaystatstooltip.h"
#include "settings/statsoverlaysettings.h"
#include "uifont.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QSurfaceFormat>

#include <algorithm>
#include <cmath>

using namespace StatsOverlay;

namespace {

// Theme.qml tokens, as in overlaystatswindow.cpp
const QColor kSurface(0x17, 0x1A, 0x20); // Theme.surface
const QColor kLine(0x2B, 0x30, 0x38);    // Theme.line
const QColor kAccent(0x39, 0xC5, 0xBB);  // Theme.accent
const QColor kText(0xEE, 0xF0, 0xEC);    // Theme.text
const QColor kTextDim(0xAE, 0xB3, 0xAB); // Theme.textDim
const QColor kShadow(0, 0, 0, 0x8C);     // Theme.shadowColor

const int kShadowOffset = 4;
const int kAccentBar = 3;
const int kPad = 10;
const int kGap = 4;
// Text wraps at this width, before scaling
const int kTextWidth = 260;

} // namespace

OverlayStatsTooltip::OverlayStatsTooltip(QWindow* parent, OverlayWindowMode windowMode)
    : OverlayRasterWindow(parent, windowMode)
{
    Q_ASSERT(windowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(windowMode, true));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    // Fixed pixel sizes, see OverlayStatsWindow
    m_TitleFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    m_TitleFont.setPixelSize(13);
    m_TitleFont.setWeight(QFont::DemiBold);

    m_BodyFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    m_BodyFont.setPixelSize(12);

    m_ValueFont.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    m_ValueFont.setPixelSize(11);
}

void OverlayStatsTooltip::setContent(Metric metric, const QString& value)
{
    m_Metric = metric;
    m_Title = StatsOverlaySettings::metricName(metric);
    m_Description = StatsOverlaySettings::metricDescription(metric);
    m_Value = value.isEmpty() ? QString() : tr("Now: %1").arg(value);
}

void OverlayStatsTooltip::setScalePercent(int percent)
{
    m_ScalePercent = clampScalePercent(percent);
}

QSize OverlayStatsTooltip::preferredSize() const
{
    const QSize body = layout(nullptr);
    return QSize(static_cast<int>(std::ceil((body.width() + kShadowOffset) * scale())),
                 static_cast<int>(std::ceil((body.height() + kShadowOffset) * scale())));
}

void OverlayStatsTooltip::repaintNow()
{
    if (!isExposed()) {
        return;
    }

    update(QRect(QPoint(), size()));
    QEvent event(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(this, &event);
}

QSize OverlayStatsTooltip::layout(QPainter* painter) const
{
    const int left = kAccentBar + kPad;
    const QRect wrapArea(0, 0, kTextWidth, 10000);
    const int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;

    const QFontMetrics titleMetrics(m_TitleFont);
    const QFontMetrics bodyMetrics(m_BodyFont);
    const QFontMetrics valueMetrics(m_ValueFont);
    const QRect title = titleMetrics.boundingRect(wrapArea, flags, m_Title);
    const QRect body = m_Description.isEmpty()
                           ? QRect()
                           : bodyMetrics.boundingRect(wrapArea, flags, m_Description);
    const QRect value =
        m_Value.isEmpty() ? QRect() : valueMetrics.boundingRect(wrapArea, flags, m_Value);

    const int width = std::max({ title.width(), body.width(), value.width() });
    int y = kPad;
    auto draw = [&](const QFont& font, const QColor& color, const QRect& rect,
                    const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        if (painter != nullptr) {
            painter->setFont(font);
            painter->setPen(color);
            painter->drawText(QRect(left, y, width, rect.height()), flags, text);
        }
        y += rect.height() + kGap;
    };
    draw(m_TitleFont, kText, title, m_Title);
    draw(m_BodyFont, kTextDim, body, m_Description);
    draw(m_ValueFont, kAccent, value, m_Value);

    return QSize(left + width + kPad, y - kGap + kPad);
}

void OverlayStatsTooltip::paintOverlay(QPainter& painter)
{
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRect(QPoint(), size()), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(scale(), scale());

    const QRect body(QPoint(), layout(nullptr));
    painter.fillRect(body.translated(kShadowOffset, kShadowOffset), kShadow);
    painter.fillRect(body, kSurface);
    painter.setPen(QPen(kLine, 1));
    painter.drawRect(body.adjusted(0, 0, -1, -1));
    painter.fillRect(QRect(0, 0, kAccentBar, body.height()), kAccent);

    layout(&painter);
}
