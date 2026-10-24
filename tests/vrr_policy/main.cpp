#include "streaming/video/vrrpolicy.h"

#include <cstdio>

static int s_Failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            s_Failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void testFrameCap()
{
    using VrrPolicy::frameCapFps;

    // Unknown refresh rates never cap
    CHECK(frameCapFps(0) == 0);
    CHECK(frameCapFps(-60) == 0);
    CHECK(frameCapFps(3) == 0);

    // At least 3 below the refresh rate, more for fast panels
    CHECK(frameCapFps(60) == 57);
    CHECK(frameCapFps(75) == 72);
    CHECK(frameCapFps(120) == 116);
    CHECK(frameCapFps(144) == 138);
    CHECK(frameCapFps(165) == 157);
    CHECK(frameCapFps(240) == 224);
    CHECK(frameCapFps(360) == 324);

    // Always inside the range, and never more than ~10% below it
    for (int hz = 24; hz <= 500; hz++) {
        const int cap = frameCapFps(hz);
        CHECK(cap > 0);
        CHECK(cap <= hz - 3);
        CHECK(cap * 10 >= hz * 8);
    }
}

static void testMinInterval()
{
    using VrrPolicy::minPresentIntervalUs;

    CHECK(minPresentIntervalUs(0) == 0);
    CHECK(minPresentIntervalUs(-1) == 0);
    CHECK(minPresentIntervalUs(1) == 1000000);
    CHECK(minPresentIntervalUs(138) == 7247); // 7246.38 rounded up
    CHECK(minPresentIntervalUs(1000000) == 1);

    // Rounding up means the cap is never exceeded
    for (int fps = 1; fps <= 1000; fps++) {
        CHECK(minPresentIntervalUs(fps) * static_cast<uint64_t>(fps) >= 1000000u);
    }
}

static void testDisplayMode()
{
    using VrrPolicy::displayModeSuitsStream;

    // Without VRR: the stream FPS must divide the refresh rate (the old rule)
    CHECK(displayModeSuitsStream(120, 60, false));
    CHECK(displayModeSuitsStream(60, 60, false));
    CHECK(!displayModeSuitsStream(144, 60, false));
    CHECK(!displayModeSuitsStream(60, 120, false));
    CHECK(!displayModeSuitsStream(60, 0, false));

    // With VRR: any real refresh rate works
    CHECK(displayModeSuitsStream(144, 60, true));
    CHECK(displayModeSuitsStream(60, 120, true));
    CHECK(!displayModeSuitsStream(0, 60, true));
}

static void testDecide()
{
    using VrrPolicy::decide;

    // VRR off keeps the old behaviour exactly
    {
        auto d = decide({ true, true, false, 60, 60 });
        CHECK(d.enableVsync && d.enableFramePacing && !d.enableVrr);
        CHECK(d.presentCapFps == 0 && !d.streamAboveVrrRange);
    }
    {
        auto d = decide({ true, false, false, 60, 144 });
        CHECK(d.enableVsync && !d.enableFramePacing && !d.enableVrr);
    }
    {
        // Frame pacing needs V-Sync
        auto d = decide({ false, true, false, 60, 60 });
        CHECK(!d.enableVsync && !d.enableFramePacing && !d.enableVrr);
    }
    {
        // V-Sync is forced off when the stream exceeds the refresh rate plus 5
        auto d = decide({ true, true, false, 120, 60 });
        CHECK(!d.enableVsync && !d.enableFramePacing);
        auto edge = decide({ true, true, false, 65, 60 });
        CHECK(edge.enableVsync && edge.enableFramePacing);
        auto over = decide({ true, true, false, 66, 60 });
        CHECK(!over.enableVsync);
    }

    // VRR on overrides V-Sync and frame pacing whatever they are set to
    for (int vsync = 0; vsync < 2; vsync++) {
        for (int pacing = 0; pacing < 2; pacing++) {
            auto d = decide({ vsync != 0, pacing != 0, true, 120, 144 });
            CHECK(!d.enableVsync && !d.enableFramePacing && d.enableVrr);
            CHECK(d.presentCapFps == 138);
            CHECK(!d.streamAboveVrrRange);
        }
    }
    {
        // A stream at or above the cap leaves the VRR range
        CHECK(decide({ true, false, true, 144, 144 }).streamAboveVrrRange);
        CHECK(decide({ true, false, true, 139, 144 }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 138, 144 }).streamAboveVrrRange);
        CHECK(decide({ true, false, true, 60, 60 }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 57, 60 }).streamAboveVrrRange);
    }
    {
        // Unknown refresh rate: no cap and no warning
        auto d = decide({ true, false, true, 120, 0 });
        CHECK(d.enableVrr && d.presentCapFps == 0 && !d.streamAboveVrrRange);
    }
}

static void testLimiter()
{
    using VrrPolicy::FrameLimiter;

    {
        // Disabled limiter never waits
        FrameLimiter limiter;
        CHECK(!limiter.isEnabled());
        limiter.onPresented(1000);
        CHECK(limiter.waitUs(1001) == 0);
    }

    FrameLimiter limiter(138);
    CHECK(limiter.isEnabled());
    CHECK(limiter.minIntervalUs() == 7247);

    // The first frame is presented immediately
    CHECK(limiter.waitUs(5000000) == 0);
    limiter.onPresented(5000000);

    // A frame arriving 2 ms later waits for the rest of the interval
    CHECK(limiter.waitUs(5002000) == 5247);
    // Exactly at the interval it goes now
    CHECK(limiter.waitUs(5007247) == 0);
    // Late frames are never held
    CHECK(limiter.waitUs(5020000) == 0);

    // Steady 120 FPS arrivals (8333 us apart) never wait at a 138 FPS cap
    uint64_t now = 6000000;
    limiter.onPresented(now);
    for (int i = 0; i < 1000; i++) {
        now += 8333;
        CHECK(limiter.waitUs(now) == 0);
        limiter.onPresented(now);
    }

    // A burst of three frames at once is spread out to the cap
    now += 50000;
    CHECK(limiter.waitUs(now) == 0);
    limiter.onPresented(now);
    uint64_t clock = now;
    for (int i = 0; i < 2; i++) {
        const uint64_t wait = limiter.waitUs(clock);
        CHECK(wait == 7247);
        clock += wait;
        CHECK(limiter.waitUs(clock) == 0);
        limiter.onPresented(clock);
    }
    CHECK(clock - now == 2 * 7247);

    // A clock that goes backwards never blocks
    CHECK(limiter.waitUs(clock - 1) == 0);
}

int main()
{
    testFrameCap();
    testMinInterval();
    testDisplayMode();
    testDecide();
    testLimiter();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d VRR policy check(s) failed\n", s_Failures);
        return 1;
    }
    std::puts("VRR policy tests passed");
    return 0;
}
