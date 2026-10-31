// Tests of the responsive panel layout (overlaystatslayout.h): labels are never
// clipped, the fallbacks happen in order, and the Detailed preset's default
// size shows everything at 100% with the overlay's real fonts.

#include "panel_layout_tests.h"

#include "streaming/video/overlaystatsfonts.h"
#include "streaming/video/overlaystatslayout.h"

#include <QDir>
#include <QFontDatabase>

using namespace StatsOverlay;

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

// Monospace stand-in: every character has the same width
class FakeMeasurer : public TextMeasurer
{
public:
    int width(TextRole role, const QString& text) const override
    {
        const int perChar = role == TextRole::Label ? 7 : role == TextRole::Value ? 8 : 17;
        return int(text.size()) * perChar;
    }
    int height(TextRole role) const override
    {
        return role == TextRole::Label ? 13 : role == TextRole::Value ? 19 : 40;
    }
};

PanelCell cell(Metric metric, const char* label, const char* shortLabel, const QString& value)
{
    return { metric, QString::fromUtf8(label), QString::fromUtf8(shortLabel), value };
}

// What the Detailed preset shows, with wide but realistic values
PanelContent detailedContent()
{
    PanelContent content;
    content.header = true;
    content.fpsValue = QStringLiteral("120");
    content.fpsLabel = QStringLiteral("FPS");
    content.headerLines = { QStringLiteral("1% low 118  0.1% low 97"),
                            QStringLiteral("3840x2160@120 HEVC Main10"),
                            QStringLiteral("D3D11VA · D3D11") };
    content.cells = {
        cell(Metric::Rtt, "RTT", "", QStringLiteral("12 ± 3 ms")),
        cell(Metric::Jitter, "Jitter", "Jit", QStringLiteral("0.42 ms")),
        cell(Metric::Bitrate, "Bitrate", "Rate", QStringLiteral("148.7 / 150 Mbps")),
        cell(Metric::Loss, "Loss", "", QStringLiteral("0.00%")),
        cell(Metric::Fec, "FEC", "", QStringLiteral("12% parity · 3 recovered, 0 lost")),
        cell(Metric::PacerDrops, "Pacer drops", "Drops", QStringLiteral("0.0%")),
        cell(Metric::DisplayHz, "Display", "Disp", QStringLiteral("144 Hz / 120 fps")),
        cell(Metric::Abr, "ABR", "", QStringLiteral("Off")),
        cell(Metric::SessionTime, "Time", "", QStringLiteral("1:02:03")),
    };
    content.graph = true;
    content.graphTitle = QStringLiteral("Frame time");
    content.graphStats = QStringLiteral("avg 8.3 · p99 12.9 ms");
    content.latencyBar = true;
    content.latencyTitle = QStringLiteral("Estimated latency");
    content.latencyValue = QStringLiteral("~23.4 ms");
    content.legend = { QStringLiteral("Host 4.1"), QStringLiteral("Network 6.0"),
                       QStringLiteral("Decode 1.2"), QStringLiteral("Queue 0.4"),
                       QStringLiteral("Render 0.8") };
    content.emptyText = QStringLiteral("No metrics selected");
    return content;
}

PanelCell compactCell(Metric metric)
{
    switch (metric) {
    case Metric::Resolution:
        return cell(metric, "Stream", "Res", QStringLiteral("3840x2160@120 HEVC"));
    case Metric::Fps:
        return cell(metric, "Rx/De/Rd", "Rx/De/Rd", QStringLiteral("120/120/120"));
    case Metric::OnePercentLow:
        return cell(metric, "1% low", "", QStringLiteral("118"));
    case Metric::Latency:
        return cell(metric, "Latency", "Lat", QStringLiteral("~23.4 ms"));
    case Metric::Bitrate:
        return cell(metric, "Bitrate", "Rate", QStringLiteral("148.7 / 150 Mbps"));
    case Metric::Loss:
        return cell(metric, "Loss", "", QStringLiteral("0.00%"));
    case Metric::Rtt:
        return cell(metric, "RTT", "", QStringLiteral("12 ± 3 ms"));
    case Metric::Jitter:
        return cell(metric, "Jitter", "Jit", QStringLiteral("0.42 ms"));
    case Metric::Host:
        return cell(metric, "Host", "", QStringLiteral("4.1 ms"));
    case Metric::Decode:
        return cell(metric, "Decode", "Dec", QStringLiteral("1.2 ms"));
    case Metric::Queue:
        return cell(metric, "Queue", "", QStringLiteral("0.4 ms"));
    case Metric::Render:
        return cell(metric, "Render", "Rend", QStringLiteral("0.8 ms"));
    case Metric::PacerDrops:
        return cell(metric, "Pacer drops", "Drops", QStringLiteral("0.0%"));
    default:
        return cell(metric, "Other", "", QStringLiteral("—"));
    }
}

QString labelOf(const PanelContent& content, const PlacedCell& placed, const PanelLayout& layout)
{
    if (placed.index < 0) {
        return layout.moreText;
    }
    const PanelCell& c = content.cells[placed.index];
    return placed.shortLabel ? c.shortLabel : c.label;
}

// Every label at full width, every rect inside the panel, no cells overlapping
bool checkLayout(const PanelContent& content, const PanelLayout& layout, const TextMeasurer& m,
                 const QString& name, QTextStream& err)
{
    bool ok = true;
    const QRect panel(QPoint(), layout.size);

    for (int i = 0; i < layout.cells.size(); i++) {
        const PlacedCell& placed = layout.cells[i];
        const QString label = labelOf(content, placed, layout);
        ok &= require(placed.labelRect.width() >= m.width(TextRole::Label, label),
                      QStringLiteral("%1: label '%2' is clipped (%3 < %4)")
                          .arg(name, label)
                          .arg(placed.labelRect.width())
                          .arg(m.width(TextRole::Label, label)),
                      err);
        ok &= require(panel.contains(placed.rect),
                      QStringLiteral("%1: cell '%2' is outside the panel").arg(name, label), err);
        for (int j = i + 1; j < layout.cells.size(); j++) {
            ok &= require(!placed.rect.intersects(layout.cells[j].rect),
                          QStringLiteral("%1: cell '%2' overlaps '%3'")
                              .arg(name, label, labelOf(content, layout.cells[j], layout)),
                          err);
        }
    }

    if (layout.graphShown) {
        ok &= require(m.width(TextRole::Label, content.graphTitle) <= layout.graphTitleRect.width(),
                      QStringLiteral("%1: graph title is clipped").arg(name), err);
        ok &= require(panel.contains(layout.graphRect),
                      QStringLiteral("%1: graph is outside the panel").arg(name), err);
    }
    if (layout.latencyShown) {
        ok &= require(m.width(TextRole::Label, content.latencyTitle) <=
                          layout.latencyTitleRect.width(),
                      QStringLiteral("%1: latency title is clipped").arg(name), err);
        ok &= require(panel.contains(layout.barRect),
                      QStringLiteral("%1: latency bar is outside the panel").arg(name), err);
    }
    if (content.header) {
        ok &= require(layout.fpsLabelRect.width() >= m.width(TextRole::Label, content.fpsLabel) &&
                          panel.contains(layout.fpsLabelRect),
                      QStringLiteral("%1: FPS label is clipped").arg(name), err);
    }
    return ok;
}

bool everythingShown(const PanelLayout& layout, const PanelContent& content)
{
    return layout.hiddenCount == 0 && layout.cells.size() == content.cells.size() &&
           layout.graphShown == content.graph && layout.latencyShown == content.latencyBar;
}

bool testDefaultSize(const TextMeasurer& m, const QString& fonts, QTextStream& err)
{
    bool ok = true;
    const PanelContent content = detailedContent();
    const PanelLayout layout = layoutPanel(content, QSize(DefaultDetailedWidth, 0), m);
    const QString name = QStringLiteral("Detailed default size (%1)").arg(fonts);

    ok &= require(layout.fits, name + QStringLiteral(": does not fit"), err);
    ok &= require(everythingShown(layout, content),
                  name + QStringLiteral(": something was left out"), err);
    ok &= require(!layout.shortLabels, name + QStringLiteral(": uses short labels"), err);
    ok &= require(layout.graphHeight == PanelSpacing::GraphHeight,
                  name + QStringLiteral(": graph was shrunk"), err);
    ok &= checkLayout(content, layout, m, name, err);

    // The same layout, asked for at exactly its own size, does not change
    const PanelLayout fixed = layoutPanel(content, layout.size, m);
    ok &= require(fixed.fits && everythingShown(fixed, content) && !fixed.shortLabels &&
                      fixed.size == layout.size,
                  name + QStringLiteral(": not stable at its own size"), err);

    // The default size is never below the minimum
    const QSize minimum = minimumPanelSize(content, m);
    ok &=
        require(minimum.width() <= DefaultDetailedWidth && minimum.height() <= layout.size.height(),
                name + QStringLiteral(": default is below the minimum size"), err);
    return ok;
}

bool testCompactStrip(const TextMeasurer& m, const QString& fonts, QTextStream& err,
                      bool inlineAtDefault = true)
{
    const QVector<Metric> expected = {
        Metric::Resolution, Metric::Fps,    Metric::OnePercentLow, Metric::Latency, Metric::Bitrate,
        Metric::Rtt,        Metric::Jitter, Metric::Loss,          Metric::Host,    Metric::Decode,
        Metric::Queue,      Metric::Render, Metric::PacerDrops,
    };
    bool ok = require(compactMetrics() == expected,
                      QStringLiteral("Compact does not preserve the legacy stats and timing "
                                     "details"),
                      err);

    PanelContent content;
    content.compactStrip = true;
    for (Metric metric : compactMetrics()) {
        content.cells.append(compactCell(metric));
    }
    const QString name = QStringLiteral("Compact default strip (%1)").arg(fonts);
    const PanelLayout wide = layoutPanel(content, QSize(DefaultCompactWidth, 0), m);
    ok &=
        require(wide.fits && wide.hiddenCount == 0 && wide.cells.size() == compactMetrics().size(),
                name + QStringLiteral(": metrics were dropped"), err);
    if (inlineAtDefault) {
        ok &= require(wide.style == CellStyle::Inline,
                      name + QStringLiteral(": labels are not inline"), err);
    }
    int rows = 0;
    int previousTop = -1;
    for (const PlacedCell& placed : wide.cells) {
        if (placed.rect.top() != previousTop) {
            rows++;
            previousTop = placed.rect.top();
        }
    }
    if (inlineAtDefault && !wide.cells.isEmpty()) {
        ok &= require(rows == 1,
                      QStringLiteral("%1: default has %2 rows across %3 columns at %4 px")
                          .arg(name)
                          .arg(rows)
                          .arg(wide.columns)
                          .arg(wide.size.width()),
                      err);
    }
    ok &= checkLayout(content, wide, m, name, err);

    const PanelLayout narrow = layoutPanel(content, QSize(MinPanelWidth, 0), m);
    ok &= require(
        narrow.fits && narrow.hiddenCount == 0 && narrow.cells.size() == compactMetrics().size() &&
            narrow.style == CellStyle::Stacked,
        QStringLiteral("narrow Compact did not stack labels while preserving metrics"), err);
    ok &= checkLayout(content, narrow, m, QStringLiteral("narrow Compact"), err);
    return ok;
}

bool testFallbackOrder(QTextStream& err)
{
    bool ok = true;
    const FakeMeasurer m;
    const PanelContent content = detailedContent();
    const int width = DefaultDetailedWidth;
    const PanelLayout natural = layoutPanel(content, QSize(width, 0), m);

    // 1. A wide panel spreads the cells over at least as many columns, with
    // fewer cells spanning two, so it is shorter
    const PanelLayout wide = layoutPanel(content, QSize(900, 0), m);
    ok &= require(wide.columns >= natural.columns && wide.size.height() < natural.size.height(),
                  QStringLiteral("wide panel is not shorter (%1x%2 in %3 columns, "
                                 "%4x%5 in %6)")
                      .arg(wide.size.width())
                      .arg(wide.size.height())
                      .arg(wide.columns)
                      .arg(natural.size.width())
                      .arg(natural.size.height())
                      .arg(natural.columns),
                  err);
    ok &= checkLayout(content, wide, m, QStringLiteral("wide"), err);

    // 2-3. Narrower panels stack labels, then shorten them; never clip them
    for (int w = width; w >= minimumPanelSize(content, m).width(); w -= 7) {
        const PanelLayout narrow = layoutPanel(content, QSize(w, 0), m);
        ok &= require(narrow.fits, QStringLiteral("width %1 does not fit").arg(w), err);
        ok &= require(everythingShown(narrow, content),
                      QStringLiteral("width %1 left something out with unlimited height").arg(w),
                      err);
        ok &= checkLayout(content, narrow, m, QStringLiteral("width %1").arg(w), err);
    }
    const PanelLayout minWidth =
        layoutPanel(content, QSize(minimumPanelSize(content, m).width(), 0), m);
    ok &= require(minWidth.style == CellStyle::Stacked && minWidth.shortLabels,
                  QStringLiteral("the narrowest panel should stack short labels"), err);

    // More height than needed goes to the graph
    const PanelLayout tall = layoutPanel(content, QSize(width, natural.size.height() + 40), m);
    ok &= require(tall.fits && everythingShown(tall, content) &&
                      tall.graphHeight == PanelSpacing::GraphHeight + 40,
                  QStringLiteral("spare height did not go to the graph"), err);
    ok &= checkLayout(content, tall, m, QStringLiteral("tall"), err);

    // A little less height still shows everything (shorter labels or a
    // smaller graph)
    const int shorter = natural.size.height() - 20;
    const PanelLayout squeezed = layoutPanel(content, QSize(width, shorter), m);
    ok &= require(squeezed.fits && everythingShown(squeezed, content) &&
                      squeezed.size.height() == shorter,
                  QStringLiteral("a slightly shorter panel left something out"), err);
    ok &= checkLayout(content, squeezed, m, QStringLiteral("squeezed"), err);

    // 4. With labels that have no short form, the graph shrinks before
    // anything is dropped
    PanelContent graphOnly = content;
    graphOnly.cells.resize(2);
    for (PanelCell& c : graphOnly.cells) {
        c.shortLabel.clear();
    }
    const PanelLayout graphNatural = layoutPanel(graphOnly, QSize(width, 0), m);
    const PanelLayout graphSqueezed =
        layoutPanel(graphOnly, QSize(width, graphNatural.size.height() - 20), m);
    ok &= require(graphSqueezed.fits && everythingShown(graphSqueezed, graphOnly) &&
                      graphSqueezed.graphHeight == PanelSpacing::GraphHeight - 20,
                  QStringLiteral("graph did not shrink before dropping metrics"), err);

    // 5. Much less height leaves out the lowest-priority metrics, with "+N"
    const int tight = natural.size.height() - 120;
    const PanelLayout dropped = layoutPanel(content, QSize(width, tight), m);
    ok &= require(dropped.fits && dropped.hiddenCount > 0 &&
                      dropped.moreText == QStringLiteral("+%1").arg(dropped.hiddenCount),
                  QStringLiteral("nothing was dropped at height %1").arg(tight), err);
    bool sessionTimeShown = false;
    bool rttShown = false;
    bool markerShown = false;
    for (const PlacedCell& placed : dropped.cells) {
        if (placed.index < 0) {
            markerShown = true;
        } else if (content.cells[placed.index].metric == Metric::SessionTime) {
            sessionTimeShown = true;
        } else if (content.cells[placed.index].metric == Metric::Rtt) {
            rttShown = true;
        }
    }
    ok &= require(markerShown && !sessionTimeShown && rttShown,
                  QStringLiteral("dropped metrics are not the lowest-priority ones"), err);
    ok &= checkLayout(content, dropped, m, QStringLiteral("dropped"), err);

    // The minimum size still fits, with only the header and the marker
    const QSize minimum = minimumPanelSize(content, m);
    const PanelLayout smallest = layoutPanel(content, minimum, m);
    ok &= require(smallest.fits, QStringLiteral("the minimum size does not fit"), err);
    ok &= checkLayout(content, smallest, m, QStringLiteral("minimum"), err);

    // Below the minimum, the layout says so
    const PanelLayout tooSmall =
        layoutPanel(content, QSize(minimum.width(), minimum.height() - 10), m);
    ok &= require(!tooSmall.fits, QStringLiteral("below the minimum still claims to fit"), err);

    ok &= require(clampPanelSize(QSize(10, 5000), minimum, QSize(800, 600)) ==
                      QSize(minimum.width(), 600),
                  QStringLiteral("size clamping"), err);
    return ok;
}

bool testEmptyAndCustom(QTextStream& err)
{
    bool ok = true;
    const FakeMeasurer m;

    PanelContent empty;
    empty.emptyText = QStringLiteral("No metrics selected");
    const PanelLayout layout = layoutPanel(empty, QSize(DefaultCustomWidth, 0), m);
    ok &= require(layout.emptyRect.isValid() && layout.cells.isEmpty() && layout.fits,
                  QStringLiteral("empty panel"), err);

    PanelContent custom;
    custom.cells = { cell(Metric::Fps, "FPS", "", QStringLiteral("120")),
                     cell(Metric::Latency, "Latency", "Lat", QStringLiteral("~23 ms")) };
    const PanelLayout two = layoutPanel(custom, QSize(DefaultCustomWidth, 0), m);
    ok &= require(two.fits && two.cells.size() == 2 && two.columns == 2,
                  QStringLiteral("two cells should share a row"), err);
    ok &= checkLayout(custom, two, m, QStringLiteral("custom"), err);

    ok &= require(stableText(QStringLiteral("12.5 ms")) == QLatin1String("00.0 ms"),
                  QStringLiteral("stable text"), err);
    ok &= require(presetIsResizable(Preset::Compact) && presetIsResizable(Preset::Detailed) &&
                      presetIsResizable(Preset::Custom) && !presetIsResizable(Preset::Minimal),
                  QStringLiteral("resizable presets"), err);
    return ok;
}

bool loadOverlayFonts(QTextStream& err)
{
    const QDir fonts(QStringLiteral(SOURCE_DIR "/../../app/res/fonts"));
    bool ok = true;
    for (const char* file : { "DMMono-Regular.ttf", "Manrope-Regular.ttf", "Manrope-SemiBold.ttf",
                              "Manrope-ExtraBold.ttf" }) {
        ok &= require(QFontDatabase::addApplicationFont(fonts.filePath(QLatin1String(file))) >= 0,
                      QStringLiteral("could not load %1").arg(QLatin1String(file)), err);
    }
    return ok;
}

} // namespace

bool runPanelLayoutTests(QTextStream& err)
{
    bool ok = true;
    ok &= testDefaultSize(FakeMeasurer(), QStringLiteral("fixed-width"), err);
    ok &= testCompactStrip(FakeMeasurer(), QStringLiteral("fixed-width"), err);
    ok &= testFallbackOrder(err);
    ok &= testEmptyAndCustom(err);

    // Load the bundled fallback, then exercise the resolved platform font.
    // Exact geometry is not assumed when a native font replaces that fallback.
    if (loadOverlayFonts(err)) {
        const FontTextMeasurer real(overlayFonts());
        ok &= testDefaultSize(real, QStringLiteral("native Semibold or bundled fallback"), err);
        ok &= testCompactStrip(real, QStringLiteral("native Semibold or bundled fallback"), err,
                               false);
    } else {
        ok = false;
    }
    return ok;
}
