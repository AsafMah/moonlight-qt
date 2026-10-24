#include "panel_layout_tests.h"
#include "streaming/video/overlaystatspreset.h"

#include <QGuiApplication>
#include <QTextStream>

using namespace StatsOverlay;

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

bool testPresets(QTextStream& err)
{
    bool ok = true;

    Preset preset = Preset::Off;
    const Preset expected[] = { Preset::Minimal, Preset::Compact, Preset::Detailed, Preset::Custom,
                                Preset::Off };
    for (Preset next : expected) {
        preset = nextPreset(preset);
        ok &= require(preset == next, QStringLiteral("preset cycle order is wrong"), err);
    }

    for (Preset p :
         { Preset::Off, Preset::Minimal, Preset::Compact, Preset::Detailed, Preset::Custom }) {
        ok &= require(presetFromId(presetId(p), Preset::Off) == p,
                      QStringLiteral("preset %1 did not round-trip").arg(presetId(p)), err);
    }
    ok &= require(presetFromId(QStringLiteral("DETAILED"), Preset::Off) == Preset::Detailed,
                  QStringLiteral("preset ids are not case-insensitive"), err);
    ok &= require(presetFromId(QStringLiteral("bogus"), Preset::Compact) == Preset::Compact,
                  QStringLiteral("unknown preset did not fall back"), err);

    return ok;
}

bool testMetricLists(QTextStream& err)
{
    bool ok = true;

    for (Metric metric : allMetrics()) {
        const QString id = metricId(metric);
        ok &= require(!id.isEmpty() && !id.contains(QLatin1Char(',')) && metricFromId(id) == metric,
                      QStringLiteral("metric id '%1' is not storable").arg(id), err);
    }
    ok &= require(allMetrics().size() == MetricCount, QStringLiteral("metric count"), err);

    // Unknown ids and duplicates are dropped; the result is in display order
    const QVector<Metric> parsed = parseMetricList(QStringLiteral("graph, fps,bogus,fps,LOW1"));
    ok &= require(
        parsed == QVector<Metric>({ Metric::Fps, Metric::OnePercentLow, Metric::FrameTimeGraph }),
        QStringLiteral("metric list parsing"), err);
    ok &= require(formatMetricList(parsed) == QLatin1String("fps,low1,graph"),
                  QStringLiteral("metric list formatting"), err);
    ok &= require(parseMetricList(QString()).isEmpty() && formatMetricList({}).isEmpty(),
                  QStringLiteral("empty metric list"), err);
    ok &=
        require(parseMetricList(formatMetricList(defaultCustomMetrics())) == defaultCustomMetrics(),
                QStringLiteral("default metrics are not in display order"), err);

    return ok;
}

bool testGrades(QTextStream& err)
{
    bool ok = true;
    const Thresholds t;

    ok &= require(gradeAbove(5, t.decodeWarnMs, t.decodeBadMs) == Grade::Good &&
                      gradeAbove(6, t.decodeWarnMs, t.decodeBadMs) == Grade::Warn &&
                      gradeAbove(8.5, t.decodeWarnMs, t.decodeBadMs) == Grade::Bad,
                  QStringLiteral("decode thresholds"), err);
    ok &= require(gradeAbove(1.5, t.lossWarnPercent, t.lossBadPercent) == Grade::Bad &&
                      gradeAbove(0, t.lossWarnPercent, t.lossBadPercent) == Grade::Good,
                  QStringLiteral("loss thresholds"), err);
    ok &= require(gradeAbove(-1, 1, 2) == Grade::Neutral, QStringLiteral("negative value"), err);

    // 1% low under 70% of the frame rate is bad
    ok &= require(gradeLow(110, 120) == Grade::Good && gradeLow(95, 120) == Grade::Warn &&
                      gradeLow(80, 120) == Grade::Bad && gradeLow(0, 120) == Grade::Neutral,
                  QStringLiteral("1% low grades"), err);

    ok &= require(gradeFps(119.5, 120) == Grade::Good && gradeFps(100, 120) == Grade::Warn &&
                      gradeFps(60, 120) == Grade::Bad && gradeFps(60, 0) == Grade::Neutral,
                  QStringLiteral("fps grades"), err);

    // Spikes are relative to the target frame time (8.33 ms at 120 FPS)
    ok &= require(gradeFrameTime(9, 120) == Grade::Good && gradeFrameTime(14, 120) == Grade::Warn &&
                      gradeFrameTime(20, 120) == Grade::Bad,
                  QStringLiteral("frame time spike grades"), err);

    return ok;
}

bool testPlacement(QTextStream& err)
{
    bool ok = true;
    const QRect parent(100, 50, 1920, 1080);
    const QSize size(300, 120);
    const int margin = 16;

    ok &= require(anchoredPosition(Anchor::TopLeft, parent, size, margin) == QPoint(116, 66),
                  QStringLiteral("top-left position"), err);
    ok &= require(anchoredPosition(Anchor::BottomRight, parent, size, margin) ==
                      QPoint(100 + 1920 - 300 - 16, 50 + 1080 - 120 - 16),
                  QStringLiteral("bottom-right position"), err);
    ok &= require(anchoredPosition(Anchor::Center, parent, size, margin) ==
                      QPoint(100 + (1920 - 300) / 2, 50 + (1080 - 120) / 2),
                  QStringLiteral("center position"), err);

    // Every anchor's own position snaps back to it
    for (int i = 0; i <= static_cast<int>(Anchor::BottomRight); i++) {
        const Anchor anchor = static_cast<Anchor>(i);
        const QRect placed(anchoredPosition(anchor, parent, size, margin), size);
        ok &=
            require(nearestAnchor(parent, placed.center()) == anchor,
                    QStringLiteral("anchor %1 does not snap to itself").arg(anchorId(anchor)), err);
        ok &= require(anchorFromId(anchorId(anchor), Anchor::Center) == anchor,
                      QStringLiteral("anchor %1 did not round-trip").arg(anchorId(anchor)), err);
    }

    // Points outside the stream are clamped to the nearest edge
    ok &= require(nearestAnchor(parent, QPoint(-500, 5000)) == Anchor::BottomLeft,
                  QStringLiteral("outside point was not clamped"), err);
    ok &= require(nearestAnchor(QRect(), QPoint(1, 1)) == Anchor::TopLeft,
                  QStringLiteral("empty parent"), err);

    ok &= require(
        clampScalePercent(10) == MinScalePercent && clampScalePercent(500) == MaxScalePercent &&
            clampOpacityPercent(0) == MinOpacityPercent && clampOpacityPercent(100) == 100,
        QStringLiteral("scale and opacity limits"), err);

    ok &= require(displayKey(QStringLiteral("\\\\.\\DISPLAY1")) == QLatin1String("____DISPLAY1") &&
                      displayKey(QStringLiteral("DP-1")) == QLatin1String("DP-1") &&
                      displayKey(QString()) == QLatin1String("default"),
                  QStringLiteral("display keys"), err);

    ok &= require(formatDuration(59) == QLatin1String("0:59") &&
                      formatDuration(3723) == QLatin1String("1:02:03"),
                  QStringLiteral("durations"), err);

    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    // The panel layout tests measure real fonts, which need a GUI application
    // but no display
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testPresets(err);
    ok &= testMetricLists(err);
    ok &= testGrades(err);
    ok &= testPlacement(err);
    ok &= runPanelLayoutTests(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
