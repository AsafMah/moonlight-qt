#include "streaming/streamquality.h"

#include <QCoreApplication>
#include <QTextStream>

using namespace StreamQuality;

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

QString describe(const QVector<ResolutionPreset>& presets)
{
    QStringList parts;
    for (const ResolutionPreset& preset : presets) {
        parts << QStringLiteral("%1x%2%3%4")
                     .arg(preset.size.width())
                     .arg(preset.size.height())
                     .arg(preset.native ? QStringLiteral("N") : QString())
                     .arg(preset.current ? QStringLiteral("C") : QString());
    }
    return parts.join(QLatin1Char(' '));
}

QString describe(const QVector<FpsPreset>& presets)
{
    QStringList parts;
    for (const FpsPreset& preset : presets) {
        parts << QStringLiteral("%1%2%3")
                     .arg(preset.fps)
                     .arg(preset.display ? QStringLiteral("D") : QString())
                     .arg(preset.current ? QStringLiteral("C") : QString());
    }
    return parts.join(QLatin1Char(' '));
}

bool testResolutionPresets(QTextStream& err)
{
    bool ok = true;

    // Native 1440p display merges with the 1440p preset and stays on top.
    QString got = describe(resolutionPresets(QSize(2560, 1440), QSize(1920, 1080)));
    ok &= require(got == QStringLiteral("2560x1440N 3840x2160 1920x1080C 1280x720"),
                  QStringLiteral("1440p native: ") + got, err);

    // Ultrawide native display, streaming at native.
    got = describe(resolutionPresets(QSize(3440, 1440), QSize(3440, 1440)));
    ok &= require(got == QStringLiteral("3440x1440NC 3840x2160 2560x1440 1920x1080 1280x720"),
                  QStringLiteral("ultrawide native: ") + got, err);

    // A custom current resolution is added in size order.
    got = describe(resolutionPresets(QSize(1920, 1080), QSize(1600, 900)));
    ok &= require(got == QStringLiteral("1920x1080N 3840x2160 2560x1440 1600x900C 1280x720"),
                  QStringLiteral("custom current: ") + got, err);

    // Unknown native display: only the standard presets.
    got = describe(resolutionPresets(QSize(), QSize(1280, 720)));
    ok &= require(got == QStringLiteral("3840x2160 2560x1440 1920x1080 1280x720C"),
                  QStringLiteral("unknown native: ") + got, err);

    // Out-of-range sizes are dropped.
    got = describe(resolutionPresets(QSize(20000, 10000), QSize(0, 0)));
    ok &= require(got == QStringLiteral("3840x2160 2560x1440 1920x1080 1280x720"),
                  QStringLiteral("invalid sizes: ") + got, err);
    return ok;
}

bool testFpsPresets(QTextStream& err)
{
    bool ok = true;

    QString got = describe(fpsPresets(144, 60));
    ok &= require(got == QStringLiteral("30 60C 90 120 144D"),
                  QStringLiteral("144 Hz display: ") + got, err);

    // Odd refresh rate is added; presets above it are dropped.
    got = describe(fpsPresets(165, 120));
    ok &= require(got == QStringLiteral("30 60 90 120C 144 165D"),
                  QStringLiteral("165 Hz display: ") + got, err);

    got = describe(fpsPresets(60, 60));
    ok &= require(got == QStringLiteral("30 60DC"), QStringLiteral("60 Hz display: ") + got, err);

    // The current FPS stays even above the display refresh rate.
    got = describe(fpsPresets(60, 120));
    ok &= require(got == QStringLiteral("30 60D 120C"),
                  QStringLiteral("stream above display: ") + got, err);

    // Unknown refresh rate keeps all presets.
    got = describe(fpsPresets(0, 75));
    ok &= require(got == QStringLiteral("30 60 75C 90 120 144"),
                  QStringLiteral("unknown display: ") + got, err);
    return ok;
}

bool testNamesDiffAndPath(QTextStream& err)
{
    bool ok = true;
    ok &= require(resolutionName(QSize(3840, 2160)) == QStringLiteral("4K"), "4K name", err);
    ok &= require(resolutionName(QSize(2560, 1440)) == QStringLiteral("1440p"), "1440p name", err);
    ok &= require(resolutionName(QSize(1280, 720)) == QStringLiteral("720p"), "720p name", err);
    ok &= require(resolutionName(QSize(3440, 1440)) ==
                      QStringLiteral("3440") + QChar(0x00D7) + QStringLiteral("1440"),
                  "custom name", err);

    const Settings base{ 1920, 1080, 60, 20000 };
    Settings to = base;
    Change change = diff(base, to);
    ok &= require(!change.any() && choosePath(change, true) == Path::None, "no change", err);

    to.bitrateKbps = 40000;
    change = diff(base, to);
    ok &= require(change.bitrate && !change.needsRestart() &&
                      choosePath(change, false) == Path::LiveBitrate,
                  "bitrate only is live", err);

    to = base;
    to.bitrateKbps = 0; // unknown target bitrate is not a change
    ok &= require(!diff(base, to).any(), "zero bitrate ignored", err);

    to = base;
    to.width = 2560;
    to.height = 1440;
    change = diff(base, to);
    ok &=
        require(change.resolution && !change.fps && change.needsRestart(), "resolution diff", err);
    ok &= require(choosePath(change, false) == Path::Reconnect, "resolution reconnects", err);
    ok &= require(choosePath(change, true) == Path::LiveResolution,
                  "resolution goes live when supported", err);

    to.fps = 120;
    change = diff(base, to);
    ok &= require(choosePath(change, true) == Path::Reconnect, "FPS always reconnects", err);

    to = base;
    to.height = 1200; // a height-only change still counts
    ok &= require(diff(base, to).resolution, "height-only diff", err);

    ok &= require(bitrateForSwitch(true, 20000, 80000) == 80000, "auto bitrate follows", err);
    ok &= require(bitrateForSwitch(false, 20000, 80000) == 20000, "manual bitrate kept", err);
    ok &= require(bitrateForSwitch(false, 0, 80000) == 80000, "missing bitrate uses default", err);
    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testResolutionPresets(err);
    ok &= testFpsPresets(err);
    ok &= testNamesDiffAndPath(err);

    if (!ok) {
        return 1;
    }

    out << "stream_quality=passed\n";
    return 0;
}
