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

inline QFont stripFont(int pixels = 18, bool label = false)
{
    QFont font;
    // Windows faces are not bundled or redistributed. Other platforms retain
    // the bundled, normal-width face before their script-specific fallbacks.
    font.setFamilies(QStringList{ QStringLiteral("Segoe UI"), QStringLiteral("Manrope"),
                                  QStringLiteral("Arial") } +
                     UiFont::systemHanFallbackFamilies());
    font.setPixelSize(pixels);
    font.setWeight(label ? QFont::Bold : QFont::DemiBold);
    font.setStretch(QFont::Unstretched);
    font.setHintingPreference(QFont::PreferFullHinting);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature("tnum", 1);
#endif
    return font;
}

inline OverlayFonts overlayFonts(int = 100)
{
    // Fixed pixels, then one uniform painter scale; platform point DPI must
    // not change the strip's geometry.
    return { stripFont(14, true), stripFont(), stripFont(30, true) };
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
