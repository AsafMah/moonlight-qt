// PyroWave presentation policy (pyrowavepacing.h): when the swapchain
// switches to triple buffering after delayed presents, and which frames the
// frame pacer presents when the stream is faster than the display.

#include "streaming/video/pyrowave/pyrowavepacing.h"

#include <QString>
#include <QTextStream>

using namespace PyrowavePacing;

namespace {

int g_Failures = 0;

void require(bool condition, const QString& what)
{
    if (!condition) {
        g_Failures++;
        QTextStream(stderr) << "FAIL: " << what << "\n";
    }
}

void testFrameInterval()
{
    require(frameIntervalUs(60, 60) == 16666, "60 fps on 60 Hz");
    require(frameIntervalUs(120, 60) == 16666, "a slower display sets the interval");
    require(frameIntervalUs(60, 144) == 16666, "a slower stream sets the interval");
    require(frameIntervalUs(120, 0) == 8333, "unknown display: the stream rate");
    require(frameIntervalUs(0, 0) == 16666, "both unknown: 60 Hz");
}

// Feeds n presents of the given length and returns how many asked to switch
int feed(SwapchainDepth& depth, int n, uint64_t presentUs)
{
    int switches = 0;
    for (int i = 0; i < n; i++) {
        if (depth.recordPresent(presentUs)) {
            switches++;
        }
    }
    return switches;
}

void testSwapchainDepth()
{
    {
        SwapchainDepth depth;
        depth.configure(60, 60);
        require(depth.depth() == SwapchainDepth::k_DoubleBuffered, "starts double-buffered");
        // 110% of 16.6 ms is 18.3 ms: 18 ms is on time, 19 ms delayed
        require(feed(depth, 1000, 18000) == 0, "on-time presents never switch");
        require(depth.delayedPresents() == 0, "on-time presents count nothing");
        require(feed(depth, 29, 19000) == 0, "29 delayed presents at 60 fps don't switch");
        require(feed(depth, 1, 19000) == 1, "the 30th (half a second) switches");
        require(depth.depth() == SwapchainDepth::k_TripleBuffered, "now triple-buffered");
        require(feed(depth, 100, 50000) == 0, "it switches only once");
    }
    {
        // Delayed and on-time presents cancel out, as in PlVkRenderer
        SwapchainDepth depth;
        depth.configure(60, 60);
        int switches = 0;
        for (int i = 0; i < 1000; i++) {
            switches += depth.recordPresent(i % 2 ? 30000 : 1000) ? 1 : 0;
        }
        require(switches == 0, "alternating delayed and on-time presents don't switch");
        require(depth.delayedPresents() <= 1, "alternating presents keep the count down");
    }
    {
        // A 120 fps stream on a 60 Hz display waits a whole refresh for every
        // present with V-Sync. That isn't a delay triple buffering can fix.
        SwapchainDepth depth;
        depth.configure(120, 60);
        require(feed(depth, 1000, 16666) == 0, "waiting for a slower display doesn't switch");
        require(feed(depth, 60, 20000) == 1, "over 110% of the display interval switches");
    }
}

// Runs the pacer over n frames and returns the pattern, '1' for presented
QString pattern(FramePacer& pacer, int n)
{
    QString result;
    for (int i = 0; i < n; i++) {
        result += pacer.shouldPresent() ? QLatin1Char('1') : QLatin1Char('0');
    }
    return result;
}

void testFramePacer()
{
    FramePacer pacer;

    pacer.configure(true, 120, 60);
    require(pacer.isDecimating(), "120 fps on 60 Hz decimates");
    require(pattern(pacer, 8) == QStringLiteral("10101010"), "120 on 60 Hz presents every other frame");

    pacer.configure(true, 144, 60);
    const QString p144 = pattern(pacer, 24);
    require(p144.count(QLatin1Char('1')) == 10, "144 on 60 Hz presents 10 of 24 frames");
    require(!p144.contains(QStringLiteral("11")), "presents never come in a row");
    require(p144.startsWith(QLatin1Char('1')), "the first frame is presented");

    pacer.configure(true, 240, 144);
    const QString p240 = pattern(pacer, 240);
    require(p240.count(QLatin1Char('1')) == 144, "240 on 144 Hz presents 144 of 240 frames");
    require(!p240.contains(QStringLiteral("00")), "no two frames in a row are skipped");

    pacer.configure(true, 60, 60);
    require(!pacer.isDecimating(), "a stream at the display rate isn't decimated");
    require(pattern(pacer, 10) == QStringLiteral("1111111111"), "every frame at the display rate");

    pacer.configure(true, 60, 144);
    require(pattern(pacer, 10) == QStringLiteral("1111111111"), "every frame on a faster display");

    pacer.configure(false, 120, 60);
    require(!pacer.isDecimating(), "frame pacing off never decimates");
    require(pattern(pacer, 10) == QStringLiteral("1111111111"), "every frame without frame pacing");

    pacer.configure(true, 120, 0);
    require(pattern(pacer, 10) == QStringLiteral("1111111111"), "every frame on an unknown display");
}

}

int main()
{
    testFrameInterval();
    testSwapchainDepth();
    testFramePacer();

    if (g_Failures != 0) {
        QTextStream(stderr) << g_Failures << " check(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "pyrowave_pacing: all checks passed\n";
    return 0;
}
