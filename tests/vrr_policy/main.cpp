#include "streaming/video/vrrpolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

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

static void testSourceCadence()
{
    VrrPolicy::SourceCadence cadence;
    CHECK(cadence.periodUs() == 0);
    cadence.addFrame(1000000);
    CHECK(cadence.periodUs() == 0);

    // Sender timestamps on a 180 Hz grid with every other tick skipped
    // average out to the content rate
    uint64_t t = 1000000;
    for (int i = 0; i < 40; i++) {
        t += (i % 2) ? 5556 : 11111;
        cadence.addFrame(t);
    }
    CHECK(cadence.periodUs() >= 8240 && cadence.periodUs() <= 8430);

    // A timestamp that goes backwards (RTP wrap) or jumps restarts the window
    cadence.addFrame(t - 1);
    CHECK(cadence.periodUs() == 0);
    cadence.addFrame(t + 7000);
    CHECK(cadence.periodUs() == 7001);
    cadence.addFrame(t + 7000 + 2000000);
    CHECK(cadence.periodUs() == 0);
}

static void testFrameStale()
{
    using VrrPolicy::isFrameStale;
    const uint64_t capInterval = VrrPolicy::minPresentIntervalUs(171); // 5848

    // Content above the cap: anything the cap held back is superseded
    CHECK(isFrameStale(300, 300, 2, 5556, capInterval));
    CHECK(!isFrameStale(300, 0, 2, 5556, capInterval));

    // Content below the cap: a frame held back by a pair of arrivals is still
    // presented unless it is more than two source periods old, or one period
    // plus the time the cap already held it
    CHECK(!isFrameStale(5800, 5700, 2, 7700, capInterval));
    CHECK(!isFrameStale(15400, 5000, 2, 7700, capInterval));
    CHECK(isFrameStale(15401, 5000, 2, 7700, capInterval));
    CHECK(!isFrameStale(16000, 8400, 2, 7700, capInterval));
    CHECK(isFrameStale(16101, 8400, 2, 7700, capInterval));

    // A burst after a stall: presenting all three at the cap would put the
    // newest 11.7 ms behind, more than a source period, so the oldest goes
    CHECK(isFrameStale(100, 0, 3, 7700, capInterval));
    // At 60 FPS content the same burst is caught up within a period
    CHECK(!isFrameStale(100, 0, 3, 16667, capInterval));

    // Just under the cap a kept frame would take far more than 8 frames to
    // catch up (each gains only period - cap interval), so it goes
    CHECK(isFrameStale(100, 5000, 2, 6000, capInterval));
    CHECK(!isFrameStale(100, 5000, 2, 6580, capInterval));

    // Unknown cadence: assume the content runs at the cap, so the newest wins
    CHECK(isFrameStale(5000, 5000, 2, 0, capInterval));
}

// Deterministic arrivals: a game rendering at contentFps (+-10% frame time),
// captured on the host's hostHz virtual display grid and sent over a network
// with 0.2-1.5 ms of jitter. Optionally, some frames are delivered together
// with their successor, and the connection stalls for 7-37 ms every 1-3 s.
struct Arrival
{
    uint64_t sourceUs;  // host capture time (the RTP timestamp)
    uint64_t arrivalUs; // decoded and handed to the Pacer
};

struct ArrivalModel
{
    int hostHz;
    double contentFps;
    double pairedFraction;
    bool stalls;
};

static std::vector<Arrival> generateArrivals(const ArrivalModel& m, uint64_t durationUs)
{
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto uniform = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (state >> 11) * (1.0 / 9007199254740992.0);
    };
    auto normal = [&]() {
        const double u1 = std::max(uniform(), 1e-12);
        const double u2 = uniform();
        return std::sqrt(-2 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    };

    std::vector<Arrival> out;
    const double tick = 1e6 / m.hostHz;
    double gameUs = 100000;
    uint64_t stallStart = 0, stallEnd = 0;
    uint64_t nextStall = 1500000 + static_cast<uint64_t>(uniform() * 1000000);
    while (gameUs < durationUs) {
        gameUs += 1e6 / m.contentFps * (1.0 + 0.1 * normal());
        const uint64_t source = static_cast<uint64_t>(std::ceil(gameUs / tick) * tick);
        if (!out.empty() && source <= out.back().sourceUs) {
            continue; // the host keeps only the newest frame of a tick
        }
        uint64_t arrival = source + 9000 + static_cast<uint64_t>(std::fabs(normal()) * 700);
        if (m.stalls) {
            if (source >= nextStall) {
                stallStart = source;
                stallEnd = source + 7000 + static_cast<uint64_t>(uniform() * 30000);
                nextStall = source + 1000000 + static_cast<uint64_t>(uniform() * 2000000);
            }
            if (source >= stallStart && source < stallEnd) {
                arrival = std::max<uint64_t>(arrival, stallEnd + 9000);
            }
        }
        out.push_back({ source, arrival });
    }
    for (size_t i = 0; i + 1 < out.size(); i++) {
        if (uniform() < m.pairedFraction) {
            out[i].arrivalUs = std::max(out[i].arrivalUs, out[i + 1].arrivalUs - 100);
        }
    }
    for (size_t i = 1; i < out.size(); i++) {
        out[i].arrivalUs = std::max(out[i].arrivalUs, out[i - 1].arrivalUs + 30);
    }
    return out;
}

// Mirrors Pacer's VRR render loop on a 180 Hz display (171 FPS cap) against a
// simulated clock: the bounded queue (3 frames, oldest dropped on overflow),
// the cap wait, the stale-frame rule before presenting and renderFrame()'s
// catch-up drops after it. wakeUs(now, deadline, n) models when the n-th
// sleep until deadline really ends.
struct VrrLoopResult
{
    double presentedFps;
    double dropPercent;
    uint64_t averageAgeUs;
    uint64_t p99AgeUs;
    uint64_t minGapUs;
};

using WakeModel = uint64_t (*)(uint64_t nowUs, uint64_t deadlineUs, int n);

static VrrLoopResult simulateVrrLoop(const std::vector<Arrival>& arrivals, WakeModel wakeUs)
{
    const int displayHz = 180;
    const uint64_t renderUs = 250;
    VrrPolicy::FrameLimiter limiter(VrrPolicy::frameCapFps(displayHz), displayHz);
    VrrPolicy::SourceCadence cadence;
    std::deque<size_t> queue;
    std::deque<int> queueHistory;
    size_t next = 0;
    int drops = 0;
    auto receiveUntil = [&](uint64_t nowUs) {
        while (next < arrivals.size() && arrivals[next].arrivalUs <= nowUs) {
            if (queue.size() == 3) {
                queue.pop_front();
                drops++;
            }
            cadence.addFrame(arrivals[next].sourceUs);
            queue.push_back(next++);
        }
    };

    std::vector<uint64_t> ages;
    uint64_t minGapUs = UINT64_MAX;
    uint64_t lastPresentUs = 0;
    uint64_t now = arrivals.front().arrivalUs;
    int sleeps = 0;
    while (next < arrivals.size() || !queue.empty()) {
        receiveUntil(now);
        if (queue.empty()) {
            now = arrivals[next].arrivalUs;
            continue;
        }

        const uint64_t readyUs = now;
        const uint64_t slotUs = limiter.presentSlotUs(now);
        while (now < slotUs) {
            now = wakeUs(now, slotUs, sleeps++);
            receiveUntil(now);
        }
        while (queue.size() > 1 &&
               VrrPolicy::isFrameStale(now - arrivals[queue.front()].arrivalUs, now - readyUs,
                                       static_cast<int>(queue.size()), cadence.periodUs(),
                                       limiter.minIntervalUs())) {
            queue.pop_front();
            drops++;
        }

        ages.push_back(now - arrivals[queue.front()].arrivalUs);
        queue.pop_front();
        limiter.onPresented(slotUs, now);
        if (!ages.empty() && lastPresentUs != 0) {
            minGapUs = std::min(minGapUs, now - lastPresentUs);
        }
        lastPresentUs = now;
        now += renderUs;
        receiveUntil(now);

        // renderFrame(): drop frames if too many stay queued for a while
        int dropTarget = 0;
        for (int queued : queueHistory) {
            if (queued == 0) {
                dropTarget = 2;
                break;
            }
        }
        if (queueHistory.size() == 180 / 2) {
            queueHistory.pop_front();
        }
        queueHistory.push_back(static_cast<int>(queue.size()));
        while (static_cast<int>(queue.size()) > dropTarget) {
            queue.pop_front();
            drops++;
        }
    }

    const double seconds = (arrivals.back().arrivalUs - arrivals.front().arrivalUs) / 1e6;
    uint64_t totalAgeUs = 0;
    for (uint64_t age : ages) {
        totalAgeUs += age;
    }
    std::vector<uint64_t> sorted = ages;
    std::sort(sorted.begin(), sorted.end());
    return { ages.size() / seconds, 100.0 * drops / arrivals.size(), totalAgeUs / ages.size(),
             sorted[sorted.size() * 99 / 100], minGapUs };
}

static void printLoop(const char* name, const VrrLoopResult& r)
{
    std::printf(
        "%-34s %6.1f FPS presented, %5.1f%% dropped, frame age avg %5llu us, "
        "p99 %5llu us, shortest gap %llu us\n",
        name, r.presentedFps, r.dropPercent, static_cast<unsigned long long>(r.averageAgeUs),
        static_cast<unsigned long long>(r.p99AgeUs), static_cast<unsigned long long>(r.minGapUs));
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
    const uint64_t durationUs = 10000000;

    // Content above the cap on a 180 Hz host: presents keep up with the cap
    // and only the newest frame is presented, so no backlog builds up
    const auto atCap = generateArrivals({ 180, 200, 0, false }, durationUs);
    const VrrLoopResult precise = simulateVrrLoop(atCap, highResolutionSleep);
    printLoop("180 FPS content", precise);
    CHECK(precise.presentedFps >= 167 && precise.presentedFps <= 171.5);
    CHECK(precise.averageAgeUs < 3500);
    CHECK(precise.minGapUs >= displayRefreshUs);

    // Millisecond sleeps (the fallback without high-resolution timers) can't
    // hold the cap exactly, but the newest frame is still presented and the
    // display's refresh rate is never exceeded
    const VrrLoopResult coarse = simulateVrrLoop(atCap, millisecondSleep);
    printLoop("180 FPS content, 1 ms sleeps", coarse);
    CHECK(coarse.presentedFps <= 171.5);
    CHECK(coarse.averageAgeUs < 5556);
    CHECK(coarse.minGapUs >= displayRefreshUs);

    // Content below the cap is never dropped, even when the network delivers
    // frames in pairs and the cap has to space them out
    const auto paired = generateArrivals({ 180, 130, 0.3, false }, durationUs);
    const VrrLoopResult pairedResult = simulateVrrLoop(paired, highResolutionSleep);
    printLoop("130 FPS content, 30% paired", pairedResult);
    CHECK(pairedResult.dropPercent < 0.5);
    CHECK(pairedResult.averageAgeUs < 3000);
    CHECK(pairedResult.minGapUs >= displayRefreshUs);

    // Network stalls release a burst; it is caught up without a lasting
    // backlog. A frame kept instead of dropped waits at most one cap interval
    // (plus timer overshoot), which bounds the tail.
    const auto stalls = generateArrivals({ 180, 130, 0, true }, durationUs);
    const VrrLoopResult stallResult = simulateVrrLoop(stalls, highResolutionSleep);
    printLoop("130 FPS content, network stalls", stallResult);
    CHECK(stallResult.dropPercent < 1.0);
    CHECK(stallResult.averageAgeUs < 1500);
    CHECK(stallResult.p99AgeUs < VrrPolicy::minPresentIntervalUs(171) + 1000);
    CHECK(stallResult.minGapUs >= displayRefreshUs);

    // Content just under the cap must not build a standing queue
    const auto nearCap = generateArrivals({ 1000, 165, 0, false }, durationUs);
    const VrrLoopResult nearCapResult = simulateVrrLoop(nearCap, highResolutionSleep);
    printLoop("165 FPS content, 1000 Hz host", nearCapResult);
    CHECK(nearCapResult.dropPercent < 2.0);
    CHECK(nearCapResult.averageAgeUs < 3000);
    CHECK(nearCapResult.minGapUs >= displayRefreshUs);

    // Just under the cap on a 180 Hz grid there is almost no headroom to catch
    // up after a pair, so a kept frame would leave a standing queue. There the
    // newest frame still wins, exactly as without the burst rule.
    const auto gridNearCap = generateArrivals({ 180, 170, 0.3, false }, durationUs);
    const VrrLoopResult gridNearCapResult = simulateVrrLoop(gridNearCap, highResolutionSleep);
    printLoop("170 FPS content, 30% paired", gridNearCapResult);
    CHECK(gridNearCapResult.dropPercent < 16.5);
    CHECK(gridNearCapResult.averageAgeUs < 2000);
    CHECK(gridNearCapResult.minGapUs >= displayRefreshUs);
}

int main()
{
    testFrameCap();
    testMinInterval();
    testDisplayMode();
    testDecide();
    testLimiter();
    testSourceCadence();
    testFrameStale();
    testLimiterLoop();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d VRR policy check(s) failed\n", s_Failures);
        return 1;
    }
    std::puts("VRR policy tests passed");
    return 0;
}
