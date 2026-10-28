#pragma once

#include "overlaystatslayout.h"
#include "uifont.h"

#include <QFont>
#include <QFontMetrics>

#include <algorithm>
#include <cmath>

// The performance overlay's fonts, and a TextMeasurer over them. Shared by
// OverlayStatsWindow and its layout test so both measure the same way.
namespace StatsOverlay {

struct OverlayFonts
{
    QFont label;
    QFont value;
    QFont big;
};

// Fixed pixel sizes for the same reason as OverlayToast: point sizes go
// through the platform font DPI, which differs from Qt's UI scale on Linux
inline OverlayFonts overlayFonts(int scalePercent = 100)
{
    const qreal scale = clampScalePercent(scalePercent) / 100.0;
    OverlayFonts fonts;
    fonts.label.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    // The painter scales the whole panel; preserve readable text at small zooms.
    fonts.label.setPixelSize(std::max(10, int(std::ceil(11 / scale))));
    fonts.label.setCapitalization(QFont::AllUppercase);
    fonts.label.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);

    fonts.value.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    fonts.value.setPixelSize(std::max(14, int(std::ceil(12 / scale))));
    fonts.value.setWeight(QFont::DemiBold);
    fonts.value.setStyleHint(QFont::SansSerif);

    fonts.big = fonts.value;
    fonts.big.setPixelSize(30);
    fonts.big.setWeight(QFont::Bold);
    return fonts;
}

class FontTextMeasurer : public TextMeasurer
{
public:
    explicit FontTextMeasurer(const OverlayFonts& fonts)
        : m_Label(fonts.label), m_Value(fonts.value), m_Big(fonts.big)
    {
    }

    int width(TextRole role, const QString& text) const override
    {
        // Rounded up by a pixel: horizontalAdvance() truncates fractional
        // advances, and drawText() must never elide a text measured to fit
        return metrics(role).horizontalAdvance(text) + 1;
    }

    int height(TextRole role) const override { return metrics(role).height(); }

private:
    const QFontMetrics& metrics(TextRole role) const
    {
        switch (role) {
        case TextRole::Value:
            return m_Value;
        case TextRole::Big:
            return m_Big;
        case TextRole::Label:
        default:
            return m_Label;
        }
    }

    QFontMetrics m_Label;
    QFontMetrics m_Value;
    QFontMetrics m_Big;
};

} // namespace StatsOverlay
