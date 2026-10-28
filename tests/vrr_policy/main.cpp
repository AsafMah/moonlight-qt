#include "streaming/video/vrrpolicy.h"

#include <cstdio>
#include <deque>

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
        limiter.onPresented(1000, 1000);
        CHECK(limiter.waitUs(1001) == 0);
    }

    FrameLimiter limiter(138);
    CHECK(limiter.isEnabled());
    CHECK(limiter.minIntervalUs() == 7247);

    // The first frame is presented immediately
    CHECK(limiter.waitUs(5000000) == 0);
    limiter.onPresented(5000000, 5000000);

    // A frame arriving 2 ms later waits for the rest of the interval
    CHECK(limiter.waitUs(5002000) == 5247);
    CHECK(limiter.presentSlotUs(5002000) == 5007247);
    // Exactly at the interval it goes now
    CHECK(limiter.waitUs(5007247) == 0);
    // Late frames are never held
    CHECK(limiter.waitUs(5020000) == 0);

    // Steady 120 FPS arrivals (8333 us apart) never wait at a 138 FPS cap
    uint64_t now = 6000000;
    limiter.onPresented(now, now);
    for (int i = 0; i < 1000; i++) {
        now += 8333;
        CHECK(limiter.waitUs(now) == 0);
        limiter.onPresented(now, now);
    }

    // A burst of three frames at once is spread out to the cap
    now += 50000;
    CHECK(limiter.waitUs(now) == 0);
    limiter.onPresented(now, now);
    uint64_t clock = now;
    for (int i = 0; i < 2; i++) {
        const uint64_t wait = limiter.waitUs(clock);
        CHECK(wait == 7247);
        clock += wait;
        CHECK(limiter.waitUs(clock) == 0);
        limiter.onPresented(clock, clock);
    }
    CHECK(clock - now == 2 * 7247);

    // A clock that goes backwards never blocks
    CHECK(limiter.waitUs(clock - 1) == 0);

    // A present that wakes late does not delay the schedule: the next slot is
    // one interval after the previous slot, not after the late wakeup
    {
        FrameLimiter l(138);
        l.onPresented(1000000, 1000000);
        const uint64_t slot = l.presentSlotUs(1001000);
        CHECK(slot == 1007247);
        l.onPresented(slot, slot + 900);
        CHECK(l.presentSlotUs(slot + 1200) == slot + 7247);
    }

    // A frame that is ready after its slot restarts the schedule, so a slow
    // stretch never lets the next two frames out back to back
    {
        FrameLimiter l(138);
        l.onPresented(1000000, 1000000);
        const uint64_t late = 1000000 + 7247 + 7000;
        CHECK(l.presentSlotUs(late) == late);
        l.onPresented(late, late);
        CHECK(l.presentSlotUs(late + 100) == late + 7247);
    }

    // A stall of a whole interval or more past the slot restarts the schedule
    // from the actual present
    {
        FrameLimiter l(138);
        l.onPresented(1000000, 1000000);
        const uint64_t slot = l.presentSlotUs(1000500);
        l.onPresented(slot, slot + 20000);
        CHECK(l.presentSlotUs(slot + 20100) == slot + 20000 + 7247);
    }
}

static void testStaleQueuedFrames()
{
    using VrrPolicy::staleQueuedFrames;

    // Frames that arrived while the cap held one back are superseded
    CHECK(staleQueuedFrames(1, true) == 0);
    CHECK(staleQueuedFrames(2, true) == 1);
    CHECK(staleQueuedFrames(3, true) == 2);
    // Without a wait, the regular queue policy decides
    CHECK(staleQueuedFrames(3, false) == 0);
    CHECK(staleQueuedFrames(0, true) == 0);
}

// Mirrors Pacer's VRR render loop against a simulated clock: a 180 FPS stream
// (a frame every 5556 us) on a 180 Hz display, so a 171 FPS cap. Frames go
// into a queue of at most 3 (the oldest is dropped on overflow). wakeUs(now,
// deadline, n) models the n-th sleep: when a sleep until deadline really ends.
struct VrrLoopResult
{
    int presents;
    uint64_t averageFrameAgeUs;
    uint64_t minGapUs;
};

using WakeModel = uint64_t (*)(uint64_t nowUs, uint64_t deadlineUs, int n);

static VrrLoopResult simulateVrrLoop(WakeModel wakeUs)
{
    const int displayHz = 180;
    const uint64_t arrivalUs = 5556;
    const uint64_t durationUs = 2000000;
    const uint64_t renderUs = 250;
    VrrPolicy::FrameLimiter limiter(VrrPolicy::frameCapFps(displayHz), displayHz);
    std::deque<uint64_t> queue;
    uint64_t nextArrivalUs = 0;
    auto receiveUntil = [&](uint64_t nowUs) {
        while (nextArrivalUs <= nowUs) {
            if (queue.size() == 3) {
                queue.pop_front();
            }
            queue.push_back(nextArrivalUs);
            nextArrivalUs += arrivalUs;
        }
    };

    VrrLoopResult result = { 0, 0, UINT64_MAX };
    uint64_t totalAgeUs = 0;
    uint64_t lastPresentUs = 0;
    uint64_t now = 0;
    int sleeps = 0;
    while (now < durationUs) {
        receiveUntil(now);
        if (queue.empty()) {
            now = nextArrivalUs;
            continue;
        }

        bool waited = false;
        const uint64_t slotUs = limiter.presentSlotUs(now);
        while (now < slotUs) {
            now = wakeUs(now, slotUs, sleeps++);
            receiveUntil(now);
            waited = true;
        }
        for (int i = VrrPolicy::staleQueuedFrames(static_cast<int>(queue.size()), waited); i > 0;
             i--) {
            queue.pop_front();
        }

        totalAgeUs += now - queue.front();
        queue.pop_front();
        limiter.onPresented(slotUs, now);
        if (result.presents > 0 && now - lastPresentUs < result.minGapUs) {
            result.minGapUs = now - lastPresentUs;
        }
        lastPresentUs = now;
        result.presents++;
        now += renderUs;
    }
    result.averageFrameAgeUs = totalAgeUs / static_cast<uint64_t>(result.presents);
    return result;
}

static void printLoop(const char* name, const VrrLoopResult& r)
{
    std::printf("180 FPS stream, 171 FPS cap, %s: %d presents in 2 s, average frame age %llu us, "
                "shortest gap %llu us\n",
                name, r.presents, static_cast<unsigned long long>(r.averageFrameAgeUs),
                static_cast<unsigned long long>(r.minGapUs));
}

// Sleep() on Windows with a 1 ms timer period: the wait is rounded up to whole
// milliseconds and then ends on the next 1 ms tick
static uint64_t millisecondSleep(uint64_t nowUs, uint64_t deadlineUs, int)
{
    const uint64_t wakeUs = nowUs + (deadlineUs - nowUs + 999) / 1000 * 1000;
    return (wakeUs + 999) / 1000 * 1000;
}

// A high-resolution waitable timer. Overshoots cycle through quantiles
// measured on Windows 11 for 4.3 ms waits: min 35, median 192, p99 540, max 738.
static uint64_t highResolutionSleep(uint64_t, uint64_t deadlineUs, int n)
{
    static const uint64_t overshootUs[] = { 35, 110, 150, 170, 185, 192, 200, 230, 280, 540, 738 };
    return deadlineUs + overshootUs[n % (sizeof(overshootUs) / sizeof(overshootUs[0]))];
}

static void testLimiterLoop()
{
    const uint64_t displayRefreshUs = VrrPolicy::minPresentIntervalUs(180);

    // The Pacer sleeps on a high-resolution timer. Presents keep up with the
    // cap despite its overshoot, and only the newest frame is presented, so no
    // backlog builds up behind the cap.
    const VrrLoopResult precise = simulateVrrLoop(highResolutionSleep);
    printLoop("high-resolution sleeps", precise);
    CHECK(precise.presents >= 2 * 168);
    CHECK(precise.presents <= 2 * 171 + 1);
    CHECK(precise.averageFrameAgeUs < 5556);
    CHECK(precise.minGapUs >= displayRefreshUs);

    // Millisecond sleeps (the fallback without high-resolution timers) can't
    // hold the cap exactly, but the newest frame is still presented and the
    // display's refresh rate is never exceeded
    const VrrLoopResult coarse = simulateVrrLoop(millisecondSleep);
    printLoop("1 ms sleeps", coarse);
    CHECK(coarse.presents <= 2 * 171 + 1);
    CHECK(coarse.averageFrameAgeUs < 5556);
    CHECK(coarse.minGapUs >= displayRefreshUs);
}

int main()
{
    testFrameCap();
    testMinInterval();
    testDisplayMode();
    testDecide();
    testLimiter();
    testStaleQueuedFrames();
    testLimiterLoop();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d VRR policy check(s) failed\n", s_Failures);
        return 1;
    }
    std::puts("VRR policy tests passed");
    return 0;
}
