#include "overlaystatslayout.h"

#include <algorithm>
#include <iterator>

namespace StatsOverlay {

using namespace PanelSpacing;

namespace {

struct Attempt
{
    CellStyle style;
    bool shortLabels;
    bool elideValues; // only when a value is wider than the whole panel
};

// Steps 2 and 3 of the fallback order in the header comment
const Attempt kAttempts[] = {
    { CellStyle::Inline, false, false }, { CellStyle::Stacked, false, false },
    { CellStyle::Inline, true, false },  { CellStyle::Stacked, true, false },
    { CellStyle::Stacked, true, true },
};
const Attempt& lastAttempt()
{
    return kAttempts[std::size(kAttempts) - 1];
}

int contentLeft()
{
    return AccentBar + Pad;
}

int innerWidth(int panelWidth)
{
    return panelWidth - AccentBar - Pad * 2;
}

// What layoutPanel() leaves out, lowest priority first
struct Drops
{
    QVector<bool> cells;
    bool graph = false;
    bool latencyBar = false;

    int count() const
    {
        return int(std::count(cells.begin(), cells.end(), true)) + (graph ? 1 : 0) +
               (latencyBar ? 1 : 0);
    }
};

struct Droppable
{
    int cell;   // index into PanelContent::cells, or -1
    bool graph; // the frame time graph
    int priority;
    int position; // display order, for ties
};

QVector<Droppable> dropOrder(const PanelContent& content)
{
    QVector<Droppable> order;
    for (int i = 0; i < content.cells.size(); i++) {
        order.append({ i, false, metricPriority(content.cells[i].metric), i });
    }
    if (content.graph) {
        order.append(
            { -1, true, metricPriority(Metric::FrameTimeGraph), int(content.cells.size()) });
    }
    if (content.latencyBar) {
        order.append(
            { -1, false, metricPriority(Metric::LatencyBar), int(content.cells.size()) + 1 });
    }
    // Lowest priority first; of equals, the one shown last goes first
    std::stable_sort(order.begin(), order.end(), [](const Droppable& a, const Droppable& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.position > b.position;
    });
    return order;
}

Drops dropsFor(const PanelContent& content, const QVector<Droppable>& order, int count)
{
    Drops drops;
    drops.cells.fill(false, content.cells.size());
    for (int i = 0; i < count && i < order.size(); i++) {
        const Droppable& d = order[i];
        if (d.cell >= 0) {
            drops.cells[d.cell] = true;
        } else if (d.graph) {
            drops.graph = true;
        } else {
            drops.latencyBar = true;
        }
    }
    return drops;
}

QString moreText(int hidden)
{
    return QStringLiteral("+%1").arg(hidden);
}

// --- Header ---------------------------------------------------------------

int planHeader(const PanelContent& content, int y, int inner, const TextMeasurer& m,
               PanelLayout& layout)
{
    const int left = contentLeft();
    const int fpsWidth = m.width(TextRole::Big, stableText(content.fpsValue));
    const int fpsLabelWidth = m.width(TextRole::Label, content.fpsLabel);
    const int bigHeight = m.height(TextRole::Big);
    const int lineHeight = m.height(TextRole::Label) + 2;
    const int lines = int(content.headerLines.size());

    const int textX = left + fpsWidth + LabelGap + fpsLabelWidth + CellPadX;
    const int textWidth = left + inner - textX;

    layout.headerLineRects.clear();
    if (lines == 0 || textWidth >= MinHeaderTextWidth) {
        // Detail lines to the right of the big number
        const int height = std::max(bigHeight, lineHeight * lines);
        layout.fpsValueRect = QRect(left, y, fpsWidth, height);
        layout.fpsLabelRect = QRect(left + fpsWidth + LabelGap, y, fpsLabelWidth, height - 6);
        const int top = y + (height - lineHeight * lines) / 2;
        for (int i = 0; i < lines; i++) {
            layout.headerLineRects.append(
                QRect(textX, top + i * lineHeight, std::max(textWidth, 0), lineHeight));
        }
        return height;
    }

    // Narrow: the detail lines go under the big number
    layout.fpsValueRect = QRect(left, y, fpsWidth, bigHeight);
    layout.fpsLabelRect = QRect(left + fpsWidth + LabelGap, y, fpsLabelWidth, bigHeight - 6);
    for (int i = 0; i < lines; i++) {
        layout.headerLineRects.append(
            QRect(left, y + bigHeight + i * lineHeight, inner, lineHeight));
    }
    return bigHeight + lineHeight * lines;
}

// --- A title on the left with a value on the right ------------------------

// Returns the height; the value moves to a second line when both don't fit
int planTitleRow(const QString& title, const QString& value, int y, int inner,
                 const TextMeasurer& m, QRect& titleRect, QRect& valueRect)
{
    const int left = contentLeft();
    const int lineHeight = m.height(TextRole::Label);
    const int titleWidth = m.width(TextRole::Label, title);
    const int valueWidth = value.isEmpty() ? 0 : m.width(TextRole::Label, stableText(value));

    titleRect = QRect(left, y, inner, lineHeight);
    if (valueWidth == 0 || titleWidth + CellPadX + valueWidth <= inner) {
        valueRect = QRect(left, y, inner, lineHeight);
        return lineHeight;
    }
    valueRect = QRect(left, y + lineHeight, inner, lineHeight);
    return lineHeight * 2;
}

// --- Grid -----------------------------------------------------------------

struct GridItem
{
    int index; // -1 for the "+N" marker
    bool shortLabel;
    int labelWidth;
    int valueWidth;
};

struct GridPlan
{
    int height = 0;
    int columns = 0;
    bool labelsFit = true;
    bool valuesFit = true;
    QVector<PlacedCell> cells;
};

int spanFor(int need, int columns, int columnWidth)
{
    const int span = (need + CellPadX + columnWidth + CellPadX - 1) / (columnWidth + CellPadX);
    return std::clamp(span, 1, columns);
}

int flowRows(const QVector<int>& needs, int columns, int columnWidth)
{
    int rows = needs.isEmpty() ? 0 : 1;
    int column = 0;
    for (int need : needs) {
        const int span = spanFor(need, columns, columnWidth);
        if (column + span > columns) {
            rows++;
            column = 0;
        }
        column += span;
    }
    return rows;
}

GridPlan planGrid(const QVector<GridItem>& items, CellStyle style, int y, int inner,
                  const TextMeasurer& m)
{
    GridPlan plan;
    const int left = contentLeft();
    const int labelHeight = m.height(TextRole::Label);
    const int valueHeight = m.height(TextRole::Value);
    const int rowHeight = style == CellStyle::Inline ? std::max(labelHeight, valueHeight) + 2
                                                     : labelHeight + LabelGap + valueHeight;
    const int rowGap = style == CellStyle::Inline ? 0 : StackedRowGap;

    QVector<int> needs;
    for (const GridItem& item : items) {
        int need = item.labelWidth;
        if (item.index >= 0) {
            need = style == CellStyle::Inline ? item.labelWidth + LabelGap * 2 + item.valueWidth
                                              : std::max(item.labelWidth, item.valueWidth);
        }
        if (item.labelWidth > inner) {
            plan.labelsFit = false;
        }
        if (need > inner) {
            plan.valuesFit = false;
        }
        needs.append(std::min(need, inner));
    }

    // Step 1: as many columns as keep the grid shortest
    int columns = 1;
    int rows = int(items.size());
    for (int c = 2; c <= items.size(); c++) {
        const int columnWidth = (inner - (c - 1) * CellPadX) / c;
        if (columnWidth < MinColumnWidth) {
            break;
        }
        const int r = flowRows(needs, c, columnWidth);
        if (r < rows) {
            rows = r;
            columns = c;
        }
    }

    const int columnWidth = (inner - (columns - 1) * CellPadX) / columns;
    int row = 0;
    int column = 0;
    for (int i = 0; i < items.size(); i++) {
        const GridItem& item = items[i];
        const int span = spanFor(needs[i], columns, columnWidth);
        if (column + span > columns) {
            row++;
            column = 0;
        }

        const int x = left + column * (columnWidth + CellPadX);
        const int width = span == columns ? inner : span * columnWidth + (span - 1) * CellPadX;
        const int top = y + row * (rowHeight + rowGap);

        PlacedCell cell;
        cell.index = item.index;
        cell.shortLabel = item.shortLabel;
        cell.rect = QRect(x, top, width, rowHeight);
        if (item.index < 0) {
            cell.labelRect = QRect(x, top, std::min(item.labelWidth, width), rowHeight);
        } else if (style == CellStyle::Inline) {
            const int labelWidth = std::min(item.labelWidth, width);
            cell.labelRect = QRect(x, top, labelWidth, rowHeight);
            const int valueX = x + labelWidth + LabelGap * 2;
            cell.valueRect = QRect(valueX, top, std::max(x + width - valueX, 0), rowHeight);
        } else {
            cell.labelRect = QRect(x, top, std::min(item.labelWidth, width), labelHeight);
            cell.valueRect = QRect(x, top + labelHeight + LabelGap, width, valueHeight);
        }
        plan.cells.append(cell);
        column += span;
    }

    plan.columns = columns;
    const int usedRows = items.isEmpty() ? 0 : row + 1;
    plan.height = usedRows * rowHeight + std::max(usedRows - 1, 0) * rowGap;
    return plan;
}

// --- Whole panel ----------------------------------------------------------

PanelLayout build(const PanelContent& content, int width, const Drops& drops, int graphHeight,
                  const Attempt& attempt, const TextMeasurer& m)
{
    PanelLayout layout;
    layout.style = attempt.style;
    layout.shortLabels = attempt.shortLabels;
    layout.graphHeight = graphHeight;
    layout.hiddenCount = drops.count();
    if (layout.hiddenCount > 0) {
        layout.moreText = moreText(layout.hiddenCount);
    }

    const int inner = std::max(innerWidth(width), 1);
    const int left = contentLeft();
    const int labelHeight = m.height(TextRole::Label);
    int y = Pad;
    bool any = false;
    auto section = [&](int height) {
        y += height + SectionGap;
        any = true;
    };

    if (content.header) {
        section(planHeader(content, y, inner, m, layout));
    }

    QVector<GridItem> items;
    for (int i = 0; i < content.cells.size(); i++) {
        if (drops.cells.value(i)) {
            continue;
        }
        const PanelCell& cell = content.cells[i];
        const bool useShort = attempt.shortLabels && !cell.shortLabel.isEmpty();
        const QString& label = useShort ? cell.shortLabel : cell.label;
        items.append({ i, useShort, m.width(TextRole::Label, label),
                       m.width(TextRole::Value, stableText(cell.value)) });
    }
    if (layout.hiddenCount > 0) {
        items.append({ -1, false, m.width(TextRole::Label, layout.moreText), 0 });
    }
    if (!items.isEmpty()) {
        GridPlan grid = planGrid(items, attempt.style, y, inner, m);
        layout.fits = grid.labelsFit && (grid.valuesFit || attempt.elideValues);
        layout.valuesElided = !grid.valuesFit;
        layout.columns = grid.columns;
        layout.cells = grid.cells;
        section(grid.height);
    }

    if (content.graph && !drops.graph) {
        layout.graphShown = true;
        const int titleHeight = planTitleRow(content.graphTitle, content.graphStats, y, inner, m,
                                             layout.graphTitleRect, layout.graphStatsRect);
        layout.graphRect = QRect(left, y + titleHeight + LabelGap, inner, graphHeight);
        section(titleHeight + LabelGap + graphHeight);
    }

    if (content.latencyBar && !drops.latencyBar) {
        layout.latencyShown = true;
        const int titleHeight = planTitleRow(content.latencyTitle, content.latencyValue, y, inner,
                                             m, layout.latencyTitleRect, layout.latencyValueRect);
        layout.barRect = QRect(left, y + titleHeight + LabelGap, inner, BarHeight);

        // Legend entries flow into as many lines as they need
        const int legendTop = layout.barRect.bottom() + 1 + LabelGap;
        int lines = content.legend.isEmpty() ? 0 : 1;
        int x = 0;
        for (const QString& text : content.legend) {
            const int textWidth = m.width(TextRole::Label, stableText(text));
            const int entryWidth = Swatch + 4 + textWidth;
            if (x > 0 && x + entryWidth > inner) {
                lines++;
                x = 0;
            }
            const int lineY = legendTop + (lines - 1) * labelHeight;
            layout.legendSwatchRects.append(
                QRect(left + x, lineY + (labelHeight - Swatch) / 2, Swatch, Swatch));
            layout.legendTextRects.append(QRect(left + x + Swatch + 4, lineY,
                                                std::min(textWidth, inner - x - Swatch - 4),
                                                labelHeight));
            x += entryWidth + CellPadX;
        }
        section(titleHeight + LabelGap + BarHeight + LabelGap + lines * labelHeight);
    }

    if (!any) {
        layout.emptyRect = QRect(left, y, inner, labelHeight);
        y += labelHeight + SectionGap;
    }

    // The last section's gap becomes the bottom padding
    layout.size = QSize(width, y - SectionGap + Pad);
    return layout;
}

} // namespace

bool presetIsResizable(Preset preset)
{
    return preset == Preset::Detailed || preset == Preset::Custom;
}

int defaultPanelWidth(Preset preset)
{
    return preset == Preset::Detailed ? DefaultDetailedWidth : DefaultCustomWidth;
}

int metricPriority(Metric metric)
{
    switch (metric) {
    case Metric::Fps:
        return 100;
    case Metric::Latency:
        return 95;
    case Metric::OnePercentLow:
        return 90;
    case Metric::Loss:
        return 85;
    case Metric::Bitrate:
        return 80;
    case Metric::Resolution:
        return 75;
    case Metric::Rtt:
        return 70;
    case Metric::FrameTimeGraph:
        return 65;
    case Metric::LatencyBar:
        return 60;
    case Metric::Decode:
        return 55;
    case Metric::Jitter:
        return 50;
    case Metric::PacerDrops:
        return 45;
    case Metric::Fec:
        return 40;
    case Metric::Host:
        return 35;
    case Metric::Render:
        return 30;
    case Metric::Queue:
        return 25;
    case Metric::DisplayHz:
        return 20;
    case Metric::Abr:
        return 15;
    case Metric::SessionTime:
        return 10;
    }
    return 0;
}

QString stableText(const QString& text)
{
    QString stable = text;
    for (QChar& c : stable) {
        if (c.isDigit()) {
            c = QLatin1Char('0');
        }
    }
    return stable;
}

PanelLayout layoutPanel(const PanelContent& content, const QSize& size,
                        const TextMeasurer& measurer)
{
    const int width = size.width();
    const Drops none = dropsFor(content, {}, 0);

    if (size.height() <= 0) {
        // As tall as needed: only the label style can change
        for (const Attempt& attempt : kAttempts) {
            PanelLayout layout = build(content, width, none, GraphHeight, attempt, measurer);
            if (layout.fits) {
                return layout;
            }
        }
        return build(content, width, none, GraphHeight, lastAttempt(), measurer);
    }

    const int height = size.height();
    const QVector<Droppable> order = dropOrder(content);
    for (int dropCount = 0; dropCount <= order.size(); dropCount++) {
        const Drops drops = dropsFor(content, order, dropCount);
        const bool hasGraph = content.graph && !drops.graph;

        // Step 4 (shrink the graph) only after every label style failed
        for (int pass = 0; pass < (hasGraph ? 2 : 1); pass++) {
            for (const Attempt& attempt : kAttempts) {
                PanelLayout layout = build(content, width, drops, GraphHeight, attempt, measurer);
                if (!layout.fits) {
                    continue;
                }
                if (layout.size.height() <= height) {
                    // A taller panel than the content needs gives the spare
                    // height to the graph
                    const int spare = height - layout.size.height();
                    if (hasGraph && spare > 0) {
                        layout =
                            build(content, width, drops,
                                  std::min(GraphHeight + spare, MaxGraphHeight), attempt, measurer);
                    }
                    layout.size.setHeight(height);
                    return layout;
                }
                if (pass == 1) {
                    const int graphHeight = GraphHeight - (layout.size.height() - height);
                    if (graphHeight >= MinGraphHeight) {
                        layout = build(content, width, drops, graphHeight, attempt, measurer);
                        layout.size.setHeight(height);
                        return layout;
                    }
                }
            }
        }
    }

    // Smaller than minimumPanelSize(): keep what fits from the top
    PanelLayout layout = build(content, width, dropsFor(content, order, int(order.size())),
                               MinGraphHeight, lastAttempt(), measurer);
    layout.fits = false;
    layout.size.setHeight(height);
    return layout;
}

QSize minimumPanelSize(const PanelContent& content, const TextMeasurer& measurer)
{
    const int chrome = AccentBar + Pad * 2;
    int width = MinPanelWidth;

    // Every label, in its shortest form, has to fit on its own
    for (const PanelCell& cell : content.cells) {
        const QString& label = cell.shortLabel.isEmpty() ? cell.label : cell.shortLabel;
        width = std::max(width, chrome + measurer.width(TextRole::Label, label));
    }
    width = std::max(width, chrome + measurer.width(TextRole::Label, moreText(99)));
    if (content.header) {
        width =
            std::max(width, chrome + measurer.width(TextRole::Big, stableText(content.fpsValue)) +
                                LabelGap + measurer.width(TextRole::Label, content.fpsLabel));
    }
    if (content.graph) {
        width = std::max(width, chrome + measurer.width(TextRole::Label, content.graphTitle));
    }
    if (content.latencyBar) {
        width = std::max(width, chrome + measurer.width(TextRole::Label, content.latencyTitle));
    }

    // Everything droppable left out: the header and the "+N" marker remain,
    // in whichever label style is shortest
    const QVector<Droppable> order = dropOrder(content);
    const Drops all = dropsFor(content, order, int(order.size()));
    int height = build(content, width, all, MinGraphHeight, lastAttempt(), measurer).size.height();
    for (const Attempt& attempt : kAttempts) {
        const PanelLayout layout = build(content, width, all, MinGraphHeight, attempt, measurer);
        if (layout.fits) {
            height = std::min(height, layout.size.height());
        }
    }
    return QSize(width, height);
}

QSize clampPanelSize(const QSize& size, const QSize& minimum, const QSize& maximum)
{
    const QSize upper = maximum.expandedTo(minimum);
    return QSize(std::clamp(size.width(), minimum.width(), upper.width()),
                 std::clamp(size.height(), minimum.height(), upper.height()));
}

} // namespace StatsOverlay
