#pragma once

#include "overlaystatspreset.h"

#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

// Responsive layout of the panel presets (Compact, Detailed and Custom) of
// OverlayStatsWindow. It is a pure function of the content, the size and the
// text metrics, so it is unit tested without a window
// (tests/stats_overlay_layout), and it does not depend on where the window
// lives.
//
// Labels are never clipped. As space runs out, the layout in turn:
//  1. spreads the cells over as many columns as fit,
//  2. stacks each label above its value instead of "label  value" on one line,
//  3. switches to the short form of the labels,
//  4. shrinks the frame time graph,
//  5. leaves out the lowest-priority metrics, with a "+N" marker.
// A value is only elided when it is wider than the whole panel.
namespace StatsOverlay {

enum class TextRole
{
    Label,
    Value,
    Big, // the Detailed header's frame rate
};

class TextMeasurer
{
public:
    virtual ~TextMeasurer() = default;
    virtual int width(TextRole role, const QString& text) const = 0;
    virtual int height(TextRole role) const = 0;
};

// Spacing, in unscaled logical pixels
namespace PanelSpacing {
constexpr int AccentBar = 3;
constexpr int Pad = 8;
constexpr int CellPadX = 10;
constexpr int LabelGap = 3;
constexpr int SectionGap = 8;
constexpr int StackedRowGap = 4;
constexpr int GraphHeight = 56;
constexpr int MinGraphHeight = 24;
constexpr int MaxGraphHeight = 400; // when the panel is taller than its content
constexpr int BarHeight = 8;
constexpr int Swatch = 6;
constexpr int MinColumnWidth = 64;
// Below this, the header's detail lines go under the big frame rate
constexpr int MinHeaderTextWidth = 110;
} // namespace PanelSpacing

// Default panel widths at 100% scale; the height follows the content until the
// user resizes the panel
constexpr int DefaultDetailedWidth = 360;
constexpr int DefaultCompactWidth = 1600;
constexpr int DefaultCustomWidth = 300;
constexpr int MinPanelWidth = 160;
constexpr int MaxPanelWidth = 1600;
constexpr int MaxPanelHeight = 1600;

// Only the panel presets can be resized
bool presetIsResizable(Preset preset);
int defaultPanelWidth(Preset preset);

// Higher numbers are kept longer when the panel is too small
int metricPriority(Metric metric);

// The text with every digit replaced by '0', so a value's measured width does
// not change (and move the layout) as its numbers tick
QString stableText(const QString& text);

struct PanelCell
{
    Metric metric = Metric::Fps;
    QString label;
    QString shortLabel; // empty when the label has no short form
    QString value;
};

struct PanelContent
{
    // Compact preset: place cells at their natural inline widths, preferring
    // short labels, before falling back to the responsive grid.
    bool compactStrip = false;

    // Detailed preset: a big frame rate with detail lines next to it
    bool header = false;
    QString fpsValue;
    QString fpsLabel;
    QStringList headerLines; // details, elided rather than wrapped

    QVector<PanelCell> cells;

    bool graph = false;
    QString graphTitle;
    QString graphStats;

    bool latencyBar = false;
    QString latencyTitle;
    QString latencyValue;
    QStringList legend;

    QString emptyText; // when nothing at all is shown
};

enum class CellStyle
{
    Inline,  // label and value on one line
    Stacked, // label above value
};

struct PlacedCell
{
    int index = -1; // into PanelContent::cells; -1 is the "+N" marker
    QRect rect;
    QRect labelRect;
    QRect valueRect;
    bool shortLabel = false;
};

struct PanelLayout
{
    QSize size;
    // False when the size is below minimumPanelSize(): a label may then be
    // elided, or content cut off at the bottom
    bool fits = true;
    // A value wider than the whole panel is elided; its label still fits
    bool valuesElided = false;

    CellStyle style = CellStyle::Inline;
    bool shortLabels = false;
    int columns = 0;
    int graphHeight = 0;

    QRect fpsValueRect;
    QRect fpsLabelRect;
    QVector<QRect> headerLineRects;

    QVector<PlacedCell> cells;
    int hiddenCount = 0; // cells and graphs left out for lack of space
    QString moreText;    // "+N" when hiddenCount > 0

    bool graphShown = false;
    QRect graphTitleRect;
    QRect graphStatsRect;
    QRect graphRect;

    bool latencyShown = false;
    QRect latencyTitleRect;
    QRect latencyValueRect;
    QRect barRect;
    QVector<QRect> legendSwatchRects;
    QVector<QRect> legendTextRects;

    QRect emptyRect;
};

// Lays the content out in a panel of the given size. A height of 0 or less
// means "as tall as the content needs at this width", with nothing left out.
PanelLayout layoutPanel(const PanelContent& content, const QSize& size,
                        const TextMeasurer& measurer);

// The smallest size at which every shown label still fits
QSize minimumPanelSize(const PanelContent& content, const TextMeasurer& measurer);

QSize clampPanelSize(const QSize& size, const QSize& minimum, const QSize& maximum);

} // namespace StatsOverlay
