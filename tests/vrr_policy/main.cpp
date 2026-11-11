#include "streaming/video/framepacingpolicy.h"
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

static void testRefreshInterval()
{
    using VrrPolicy::refreshIntervalUs;

    CHECK(refreshIntervalUs(0) == 0);
    CHECK(refreshIntervalUs(-1) == 0);
    // A reported 180 Hz may really be 179.96 Hz (5556.8 us): allow 0.1% slower
    CHECK(refreshIntervalUs(180) == 5562);
    CHECK(refreshIntervalUs(60) == 16684); // covers 59.94 Hz (16683.4 us)
    for (int hz = 1; hz <= 1000; hz++) {
        CHECK(refreshIntervalUs(hz) * static_cast<uint64_t>(hz) * 1000 >= 1001000000u);
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
            // Presents are limited to the refresh rate, never below it
            CHECK(d.presentCapFps == 144);
            CHECK(!d.streamAboveVrrRange);
        }
    }
    {
        // Only a stream faster than the refresh rate leaves the VRR range
        CHECK(decide({ true, false, true, 145, 144 }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 144, 144 }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 139, 144 }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 60, 60 }).streamAboveVrrRange);
        CHECK(decide({ true, false, true, 61, 60 }).streamAboveVrrRange);
    }
    {
        // Unknown refresh rate: no cap and no warning
        auto d = decide({ true, false, true, 120, 0 });
        CHECK(d.enableVrr && d.presentCapFps == 0 && !d.streamAboveVrrRange);
        CHECK(!d.nonaryVrr);
    }

    // The Nonary pacer keeps V-Sync on (it chooses tearing or synchronized
    // presents per frame), never frame pacing, whatever they are set to
    for (int vsync = 0; vsync < 2; vsync++) {
        for (int pacing = 0; pacing < 2; pacing++) {
            auto d = decide({ vsync != 0, pacing != 0, true, 120, 144, true });
            CHECK(d.enableVsync && !d.enableFramePacing && d.enableVrr && d.nonaryVrr);
            // Kept for the fork-limiter fallback
            CHECK(d.presentCapFps == 144);
        }
    }
    {
        // Nonary has no below-refresh cap: it only drops above native refresh
        CHECK(!decide({ true, false, true, 144, 144, true }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 139, 144, true }).streamAboveVrrRange);
        CHECK(decide({ true, false, true, 145, 144, true }).streamAboveVrrRange);
        CHECK(!decide({ true, false, true, 120, 0, true }).streamAboveVrrRange);
    }
    {
        // Selecting the Nonary pacer without VRR changes nothing
        auto d = decide({ true, true, false, 60, 60, true });
        CHECK(d.enableVsync && d.enableFramePacing && !d.enableVrr && !d.nonaryVrr);
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

// Mirrors Pacer's VRR render loop on a 180 Hz display (presents at least one
// refresh apart) against a
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
    // Share of consecutive present gaps that differ by more than 2 ms, beyond
    // what the host's own capture gaps explain: the stutter the client adds
    double addedJerkPercent;
};

using WakeModel = uint64_t (*)(uint64_t nowUs, uint64_t deadlineUs, int n);

static VrrLoopResult simulateVrrLoop(const std::vector<Arrival>& arrivals, WakeModel wakeUs,
                                     bool smooth = false, int displayHz = 180,
                                     uint32_t exactNumerator = 0, uint32_t exactDenominator = 0)
{
    const uint64_t renderUs = 250;
    // The Pacer's limiter for a stream at this display's refresh rate
    const VrrPolicy::SyncDecision sync =
        VrrPolicy::decide({ false, false, true, displayHz, displayHz });
    VrrPolicy::FrameLimiter limiter =
        VrrPolicy::FrameLimiter::forDisplay(sync.presentCapFps, exactNumerator, exactDenominator);
    VrrPolicy::SourceCadence cadence;
    VrrPolicy::PlayoutClock playout;
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
            playout.observe(arrivals[next].sourceUs, arrivals[next].arrivalUs);
            queue.push_back(next++);
        }
    };

    std::vector<uint64_t> ages;
    uint64_t minGapUs = UINT64_MAX;
    uint64_t lastPresentUs = 0;
    int64_t lastSpacingErrorUs = 0;
    int jerks = 0, addedJerks = 0;
    size_t lastFrame = 0;
    uint64_t now = arrivals.front().arrivalUs;
    int sleeps = 0;
    while (next < arrivals.size() || !queue.empty()) {
        receiveUntil(now);
        if (queue.empty()) {
            now = arrivals[next].arrivalUs;
            continue;
        }

        const uint64_t readyUs = now;
        uint64_t slotUs = limiter.presentSlotUs(now);
        if (smooth) {
            slotUs = std::max(slotUs, playout.targetUs(arrivals[queue.front()].sourceUs));
        }
        while (now < slotUs) {
            now = wakeUs(now, slotUs, sleeps++);
            receiveUntil(now);
        }
        if (smooth) {
            while (queue.size() > 1 && playout.targetUs(arrivals[queue[1]].sourceUs) != 0 &&
                   playout.targetUs(arrivals[queue[1]].sourceUs) <= now) {
                queue.pop_front();
                drops++;
            }
        } else {
            while (queue.size() > 1 &&
                   VrrPolicy::isFrameStale(now - arrivals[queue.front()].arrivalUs, now - readyUs,
                                           static_cast<int>(queue.size()), cadence.periodUs(),
                                           limiter.minIntervalUs())) {
                queue.pop_front();
                drops++;
            }
        }

        const size_t frame = queue.front();
        ages.push_back(now - arrivals[frame].arrivalUs);
        queue.pop_front();
        limiter.onPresented(slotUs, now);
        if (lastPresentUs != 0) {
            minGapUs = std::min(minGapUs, now - lastPresentUs);
            // How far this gap is from the host's capture gap
            const int64_t spacingErrorUs =
                static_cast<int64_t>(now - lastPresentUs) -
                static_cast<int64_t>(arrivals[frame].sourceUs - arrivals[lastFrame].sourceUs);
            if (jerks++ > 0) {
                addedJerks += std::llabs(spacingErrorUs - lastSpacingErrorUs) > 2000;
            }
            lastSpacingErrorUs = spacingErrorUs;
        }
        lastPresentUs = now;
        lastFrame = frame;
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
        // (Smooth keeps frames queued until their time and skips this)
        while (!smooth && static_cast<int>(queue.size()) > dropTarget) {
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
    return { ages.size() / seconds,
             100.0 * drops / arrivals.size(),
             totalAgeUs / ages.size(),
             sorted[sorted.size() * 99 / 100],
             minGapUs,
             100.0 * addedJerks / std::max(1, jerks - 1) };
}

static void printLoop(const char* name, const VrrLoopResult& r)
{
    std::printf("%-40s %6.1f FPS presented, %5.1f%% dropped, frame age avg %5llu us, "
                "p99 %5llu us, shortest gap %llu us, added jerk %4.1f%%\n",
                name, r.presentedFps, r.dropPercent,
                static_cast<unsigned long long>(r.averageAgeUs),
                static_cast<unsigned long long>(r.p99AgeUs),
                static_cast<unsigned long long>(r.minGapUs), r.addedJerkPercent);
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

// The Pacer's sleeper: a high-resolution timer to just before the deadline,
// then a short spin, so it wakes within a few microseconds
static uint64_t spinSleep(uint64_t, uint64_t deadlineUs, int n)
{
    static const uint64_t overshootUs[] = { 1, 2, 3, 2, 5, 1, 8, 2, 3, 15 };
    return deadlineUs + overshootUs[n % (sizeof(overshootUs) / sizeof(overshootUs[0]))];
}

// The host sets the frame rate, so a stream at the display's refresh rate
// must not lose frames to a cap below it: every frame is presented, at least
// one refresh after the previous one
static void testRefreshRateLoop()
{
    const uint64_t displayRefreshUs = VrrPolicy::minPresentIntervalUs(180);
    const uint64_t durationUs = 10000000;

    const auto atRefresh = generateArrivals({ 180, 180, 0, false }, durationUs);
    const VrrLoopResult refreshResult = simulateVrrLoop(atRefresh, spinSleep);
    printLoop("180 Hz host, 180 FPS game", refreshResult);
    const auto above = generateArrivals({ 180, 200, 0, false }, durationUs);
    const VrrLoopResult aboveResult = simulateVrrLoop(above, spinSleep);
    printLoop("180 Hz host, 200 FPS game", aboveResult);
    for (const VrrLoopResult& r : { refreshResult, aboveResult }) {
        CHECK(r.dropPercent < 1.0);
        CHECK(r.minGapUs >= VrrPolicy::refreshIntervalUs(180));
    }
    CHECK(aboveResult.presentedFps >= 178);
    // Mean frame age before this change: 2.7 ms at 200 FPS
    CHECK(aboveResult.averageAgeUs < 3700);
    CHECK(refreshResult.averageAgeUs < 3700);

    // With the timer alone (no spin) presents still never come too close
    const VrrLoopResult timerOnly = simulateVrrLoop(above, highResolutionSleep);
    printLoop("180 Hz host, 200 FPS game, timer only", timerOnly);
    CHECK(timerOnly.minGapUs >= displayRefreshUs);
}

static void testLimiterLoop()
{
    const uint64_t displayRefreshUs = VrrPolicy::minPresentIntervalUs(180);
    const uint64_t durationUs = 10000000;

    // Content above the cap on a 180 Hz host: presents keep up with the cap
    // and only the newest frame is presented, so no backlog builds up
    const auto atCap = generateArrivals({ 180, 200, 0, false }, durationUs);
    const VrrLoopResult precise = simulateVrrLoop(atCap, spinSleep);
    printLoop("180 FPS content", precise);
    CHECK(precise.presentedFps >= 178 && precise.presentedFps <= 180.5);
    CHECK(precise.dropPercent < 1.0);
    CHECK(precise.averageAgeUs < 3500);
    CHECK(precise.minGapUs >= displayRefreshUs);

    // Millisecond sleeps (the fallback without high-resolution timers) can't
    // hold the cap exactly, but the newest frame is still presented and the
    // display's refresh rate is never exceeded
    const VrrLoopResult coarse = simulateVrrLoop(atCap, millisecondSleep);
    printLoop("180 FPS content, 1 ms sleeps", coarse);
    CHECK(coarse.presentedFps <= 180.5);
    CHECK(coarse.averageAgeUs < 5556);
    CHECK(coarse.minGapUs >= displayRefreshUs);

    // Content below the cap is never dropped, even when the network delivers
    // frames in pairs and the cap has to space them out
    const auto paired = generateArrivals({ 180, 130, 0.3, false }, durationUs);
    const VrrLoopResult pairedResult = simulateVrrLoop(paired, spinSleep);
    printLoop("130 FPS content, 30% paired", pairedResult);
    CHECK(pairedResult.dropPercent < 0.5);
    CHECK(pairedResult.averageAgeUs < 3000);
    CHECK(pairedResult.minGapUs >= displayRefreshUs);

    // Network stalls release a burst; it is caught up without a lasting
    // backlog. A frame kept instead of dropped waits at most one cap interval
    // (plus timer overshoot), which bounds the tail.
    const auto stalls = generateArrivals({ 180, 130, 0, true }, durationUs);
    const VrrLoopResult stallResult = simulateVrrLoop(stalls, spinSleep);
    printLoop("130 FPS content, network stalls", stallResult);
    CHECK(stallResult.dropPercent < 1.0);
    CHECK(stallResult.averageAgeUs < 1500);
    CHECK(stallResult.p99AgeUs < displayRefreshUs + 1000);
    CHECK(stallResult.minGapUs >= displayRefreshUs);

    // Content just under the cap must not build a standing queue
    const auto nearCap = generateArrivals({ 1000, 165, 0, false }, durationUs);
    const VrrLoopResult nearCapResult = simulateVrrLoop(nearCap, spinSleep);
    printLoop("165 FPS content, 1000 Hz host", nearCapResult);
    CHECK(nearCapResult.dropPercent < 2.0);
    CHECK(nearCapResult.averageAgeUs < 3000);
    CHECK(nearCapResult.minGapUs >= displayRefreshUs);

    // Just under the cap on a 180 Hz grid there is almost no headroom to catch
    // up after a pair, so a kept frame would leave a standing queue. There the
    // newest frame still wins, exactly as without the burst rule.
    const auto gridNearCap = generateArrivals({ 180, 170, 0.3, false }, durationUs);
    const VrrLoopResult gridNearCapResult = simulateVrrLoop(gridNearCap, spinSleep);
    printLoop("170 FPS content, 30% paired", gridNearCapResult);
    CHECK(gridNearCapResult.dropPercent < 16.5);
    CHECK(gridNearCapResult.averageAgeUs < 2000);
    CHECK(gridNearCapResult.minGapUs >= displayRefreshUs);
}

// A host capturing at exactly 120 FPS: RTP timestamps 750 ticks (90 kHz)
// apart, converted to microseconds the way Pacer does (pts * 100 / 9). Frames
// arrive 1.5 ms after capture, and every 8th (large) frame lateUs later.
static std::vector<Arrival> exact120Arrivals(uint64_t lateUs)
{
    std::vector<Arrival> out;
    for (uint64_t i = 1; i <= 3600; i++) {
        const uint64_t sourceUs = (90000 + i * 750) * 100 / 9;
        out.push_back({ sourceUs, sourceUs + 1500 + (i % 8 == 0 ? lateUs : 0) });
    }
    return out;
}

// Same drops, and frame age (avg and p99) within 1 us
static bool sameLoopResult(const VrrLoopResult& a, const VrrLoopResult& b)
{
    auto near = [](uint64_t x, uint64_t y) {
        return (x > y ? x - y : y - x) <= 1;
    };
    return a.dropPercent == b.dropPercent && near(a.averageAgeUs, b.averageAgeUs) &&
           near(a.p99AgeUs, b.p99AgeUs);
}

// The limiter keeps presents one refresh apart. With only the whole-number
// refresh rate it allows for a refresh up to 0.1% slower (119.88 Hz shows as
// 120), so a stream at exactly 120.000 FPS outran it by ~9 us per frame, and
// each late wake-up pushed the next present back too: Smooth fell a whole
// frame behind every few seconds, then dropped one (queue sawing 0.5 -> 8.7
// ms; 3 -> 12 ms in a field report). The exact refresh rate leaves no margin
// to outrun, and the gap after a present tolerates 50 us of wake-up lateness.
static void testSmoothExactRefresh()
{
    const uint64_t toleranceUs = VrrPolicy::FrameLimiter::ExactRateWakeToleranceNs / 1000;

    // Exact rates are used only within 1 Hz of the reported refresh rate
    CHECK(VrrPolicy::exactRefreshIntervalNs(120, 120000, 1000) == 8333334);
    CHECK(VrrPolicy::exactRefreshIntervalNs(120, 120000, 1001) == 8341667);
    CHECK(VrrPolicy::exactRefreshIntervalNs(119, 120000, 1001) != 0);
    CHECK(VrrPolicy::exactRefreshIntervalNs(120, 121001, 1000) == 0);
    CHECK(VrrPolicy::exactRefreshIntervalNs(120, 118999, 1000) == 0);
    CHECK(VrrPolicy::exactRefreshIntervalNs(120, 0, 0) == 0);

    for (uint64_t late : { 0ull, 3000ull }) {
        const auto arrivals = exact120Arrivals(late);

        const VrrLoopResult exact = simulateVrrLoop(arrivals, spinSleep, true, 120, 120000, 1000);
        printLoop(late ? "120 FPS on 120.000 Hz, late frames, Smooth"
                       : "120 FPS on 120.000 Hz, Smooth",
                  exact);
        CHECK(exact.dropPercent == 0);
        // Queue stays at the playout delay (late + 0.5 ms margin), no sawtooth
        CHECK(exact.p99AgeUs < late + 1000);
        CHECK(exact.minGapUs >= 8333 - toleranceUs);

        // A 119.88 Hz panel really is slower than the stream, so it still
        // drops frames like the whole-number fallback (no more often);
        // only wake-up lateness no longer adds to the lag (avg 93 us lower)
        const VrrLoopResult ntsc = simulateVrrLoop(arrivals, spinSleep, true, 120, 120000, 1001);
        const VrrLoopResult rounded = simulateVrrLoop(arrivals, spinSleep, true, 120);
        printLoop("120 FPS on 119.88 Hz, Smooth", ntsc);
        printLoop("120 FPS on \"120\" Hz (no exact rate), Smooth", rounded);
        CHECK(ntsc.dropPercent <= rounded.dropPercent && ntsc.dropPercent > 0);
        CHECK(ntsc.averageAgeUs <= rounded.averageAgeUs);
        CHECK(ntsc.p99AgeUs + 10 >= rounded.p99AgeUs && ntsc.p99AgeUs <= rounded.p99AgeUs + 10);
        CHECK(ntsc.minGapUs >= 8342 - toleranceUs);
        CHECK(rounded.minGapUs >= 8342);

        // With headroom (144 Hz) the exact rate changes nothing
        const VrrLoopResult fast = simulateVrrLoop(arrivals, spinSleep, true, 144, 144000, 1000);
        const VrrLoopResult fastRounded = simulateVrrLoop(arrivals, spinSleep, true, 144);
        printLoop("120 FPS on 144.000 Hz, Smooth", fast);
        CHECK(sameLoopResult(fast, fastRounded));
        CHECK(fast.dropPercent == 0);
        CHECK(fast.minGapUs >= 6945 - toleranceUs);
    }

    // An exact rate more than 1 Hz from the reported refresh (the window moved
    // to another display) is ignored: exactly the whole-number behaviour
    const auto arrivals = exact120Arrivals(0);
    const VrrLoopResult mismatched = simulateVrrLoop(arrivals, spinSleep, true, 120, 121001, 1000);
    const VrrLoopResult rounded = simulateVrrLoop(arrivals, spinSleep, true, 120);
    CHECK(sameLoopResult(mismatched, rounded) && mismatched.minGapUs == rounded.minGapUs);
}

static void testPlayoutClock()
{
    VrrPolicy::PlayoutClock clock;
    CHECK(clock.targetUs(1000000) == 0);

    // Host clock 3 s ahead of the client; frames take 9-11 ms to arrive
    uint64_t source = 3000000;
    for (int i = 0; i < 300; i++) {
        source += 5556;
        const uint64_t lateness = 9000 + (i % 10) * 200;
        clock.observe(source, source - 3000000 + lateness);
    }
    // The fastest path is 9 ms; the 95th percentile is 1.8 ms later than that
    CHECK(clock.delayUs() >= 2000 && clock.delayUs() <= 2600);
    CHECK(clock.targetUs(source) == source - 3000000 + 9000 + clock.delayUs());

    // A sender clock that restarts (RTP wrap) starts learning again
    clock.observe(1000, 5000000);
    CHECK(clock.targetUs(1000) == 0);
}

static void testSmoothLoop()
{
    const uint64_t displayRefreshUs = VrrPolicy::minPresentIntervalUs(180);
    const uint64_t durationUs = 10000000;

    // A game at 130 FPS on a 1000 Hz host: presenting on arrival passes the
    // network jitter to the screen; Smooth replays the host's timing for a
    // couple of milliseconds more latency
    const auto jittery = generateArrivals({ 1000, 130, 0, false }, durationUs);
    const VrrLoopResult onArrival = simulateVrrLoop(jittery, spinSleep);
    const VrrLoopResult smooth = simulateVrrLoop(jittery, spinSleep, true);
    printLoop("130 FPS, 1000 Hz host", onArrival);
    printLoop("130 FPS, 1000 Hz host, Smooth", smooth);
    CHECK(smooth.addedJerkPercent * 4 < onArrival.addedJerkPercent);
    CHECK(smooth.dropPercent < 0.5);
    CHECK(smooth.averageAgeUs < onArrival.averageAgeUs + 3000);
    CHECK(smooth.minGapUs >= displayRefreshUs);

    // Network stalls: late frames are shown on arrival and the schedule
    // catches up by dropping frames whose successor is already due
    const auto stalls = generateArrivals({ 1000, 130, 0, true }, durationUs);
    const VrrLoopResult stallSmooth = simulateVrrLoop(stalls, spinSleep, true);
    printLoop("130 FPS, 1000 Hz host, stalls, Smooth", stallSmooth);
    CHECK(stallSmooth.dropPercent < 2.0);
    CHECK(stallSmooth.averageAgeUs < 4000);
    CHECK(stallSmooth.minGapUs >= displayRefreshUs);

    // Paired delivery: Smooth spaces the pair out by sender time
    const auto paired = generateArrivals({ 180, 130, 0.3, false }, durationUs);
    const VrrLoopResult pairedSmooth = simulateVrrLoop(paired, spinSleep, true);
    printLoop("130 FPS, 30% paired, Smooth", pairedSmooth);
    CHECK(pairedSmooth.dropPercent < 0.5);
    CHECK(pairedSmooth.minGapUs >= displayRefreshUs);

    // Content above the cap: the cap still decides and the delay stays small
    const auto atCap = generateArrivals({ 180, 200, 0, false }, durationUs);
    const VrrLoopResult atCapSmooth = simulateVrrLoop(atCap, spinSleep, true);
    printLoop("180 FPS content, Smooth", atCapSmooth);
    CHECK(atCapSmooth.presentedFps >= 165);
    CHECK(atCapSmooth.averageAgeUs < 5000);
    CHECK(atCapSmooth.minGapUs >= displayRefreshUs);
}

// A 120 FPS source with network jitter: arrivals 1.5 ms after capture, give
// or take up to 1 ms (deterministic), always in order
static std::vector<Arrival> jittered120Arrivals()
{
    std::vector<Arrival> out = exact120Arrivals(0);
    for (size_t i = 0; i < out.size(); i++) {
        out[i].arrivalUs += 1000;
        out[i].arrivalUs -= static_cast<uint64_t>((i * 7919) % 2001);
        if (i > 0) {
            out[i].arrivalUs = std::max(out[i].arrivalUs, out[i - 1].arrivalUs + 30);
        }
    }
    return out;
}

struct VsyncLoopResult
{
    double dropPercent;
    // Overlay "Queue": decoded -> render starts
    uint64_t averageQueueUs;
    uint64_t p99QueueUs;
    // Of that, time held in Pacer's pacing queue
    uint64_t p99PacerHoldUs;
    // Not in "Queue": presented -> latched by the display at a V-blank
    uint64_t averageDisplayWaitUs;
};

// Mirrors Pacer with V-Sync and no VRR on D3D11 against a simulated clock:
// V-blanks every num/den Hz starting at firstVblankUs; with frame pacing,
// FramePacing::VblankGate decides when a frame leaves the pacing queue (one
// release per V-blank, otherwise held to the next; 3 frames at most). The
// render thread waits on the swapchain's frame latency waitable (max latency
// 1: signalled once the previous present was latched at a V-blank), then takes
// the newest queued frame, dropping older ones, and renders for 250 us.
static VsyncLoopResult simulateVsyncLoop(const std::vector<Arrival>& arrivals, uint32_t num,
                                         uint32_t den, bool pacing, double firstVblankUs)
{
    const double periodUs = 1e6 * den / num;
    const uint64_t renderUs = 250;
    const uint64_t never = UINT64_MAX;
    const FramePacing::VblankGate gate;
    std::deque<size_t> pacingQueue, renderQueue;
    std::vector<uint64_t> enteredRenderQueueUs(arrivals.size(), 0);
    std::vector<uint64_t> queue, hold;
    uint64_t displayWaitTotalUs = 0;
    int drops = 0;
    size_t next = 0;
    double nextVblankUs = firstVblankUs;
    while (nextVblankUs < arrivals.front().arrivalUs) {
        nextVblankUs += periodUs;
    }
    uint64_t renderReadyUs = 0;
    uint64_t latchUs = 0;
    uint64_t clockUs = 0;
    auto toRender = [&](size_t frame, uint64_t nowUs) {
        enteredRenderQueueUs[frame] = nowUs;
        if (renderQueue.size() == 3) {
            renderQueue.pop_front();
            drops++;
        }
        renderQueue.push_back(frame);
    };
    while (next < arrivals.size() || !pacingQueue.empty() || !renderQueue.empty()) {
        const uint64_t arrivalUs = next < arrivals.size() ? arrivals[next].arrivalUs : never;
        const uint64_t vblankUs = static_cast<uint64_t>(nextVblankUs);
        const uint64_t renderUsAt =
            renderQueue.empty() ? never : std::max({ renderReadyUs, latchUs, clockUs });
        const uint64_t nowUs = std::min({ arrivalUs, vblankUs, renderUsAt });
        clockUs = nowUs;
        if (nowUs == arrivalUs) {
            const size_t frame = next++;
            if (!pacing) {
                toRender(frame, nowUs);
            } else if (pacingQueue.empty() && gate.releaseOnArrival()) {
                toRender(frame, nowUs);
            } else {
                if (pacingQueue.size() == 3) {
                    pacingQueue.pop_front();
                    drops++;
                }
                pacingQueue.push_back(frame);
            }
        } else if (nowUs == vblankUs) {
            nextVblankUs += periodUs;
            if (pacing && gate.onVblank(!pacingQueue.empty())) {
                toRender(pacingQueue.front(), nowUs);
                pacingQueue.pop_front();
            }
        } else {
            while (FramePacing::presentNewestOnly(false, true, pacing, false) &&
                   renderQueue.size() > 1) {
                renderQueue.pop_front();
                drops++;
            }
            const size_t frame = renderQueue.front();
            renderQueue.pop_front();
            queue.push_back(nowUs - arrivals[frame].arrivalUs);
            hold.push_back(enteredRenderQueueUs[frame] - arrivals[frame].arrivalUs);
            const uint64_t presentUs = nowUs + renderUs;
            renderReadyUs = presentUs;
            // Latched at the first V-blank after the present
            const double vblanks = std::floor((presentUs - firstVblankUs) / periodUs) + 1;
            latchUs = static_cast<uint64_t>(firstVblankUs + vblanks * periodUs);
            displayWaitTotalUs += latchUs - presentUs;
        }
    }

    auto average = [](const std::vector<uint64_t>& v) {
        uint64_t total = 0;
        for (uint64_t x : v) {
            total += x;
        }
        return total / v.size();
    };
    auto p99 = [](std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() * 99 / 100];
    };
    return { 100.0 * drops / arrivals.size(), average(queue), p99(queue), p99(hold),
             displayWaitTotalUs / queue.size() };
}

// Worst case over where the V-blanks fall relative to the frames
static VsyncLoopResult worstVsyncLoop(const std::vector<Arrival>& arrivals, uint32_t num,
                                      uint32_t den, bool pacing)
{
    VsyncLoopResult worst = {};
    for (int phaseUs = 0; phaseUs < 8500; phaseUs += 250) {
        const VsyncLoopResult r = simulateVsyncLoop(arrivals, num, den, pacing, phaseUs);
        worst.dropPercent = std::max(worst.dropPercent, r.dropPercent);
        worst.averageQueueUs = std::max(worst.averageQueueUs, r.averageQueueUs);
        worst.p99QueueUs = std::max(worst.p99QueueUs, r.p99QueueUs);
        worst.p99PacerHoldUs = std::max(worst.p99PacerHoldUs, r.p99PacerHoldUs);
        worst.averageDisplayWaitUs = std::max(worst.averageDisplayWaitUs, r.averageDisplayWaitUs);
    }
    return worst;
}

static void printVsyncLoop(const char* name, const VsyncLoopResult& r)
{
    std::printf("%-46s %5.2f%% dropped, queue avg %5llu us, p99 %5llu us (pacer hold p99 %5llu us)"
                ", display wait avg %5llu us\n",
                name, r.dropPercent, static_cast<unsigned long long>(r.averageQueueUs),
                static_cast<unsigned long long>(r.p99QueueUs),
                static_cast<unsigned long long>(r.p99PacerHoldUs),
                static_cast<unsigned long long>(r.averageDisplayWaitUs));
}

// V-Sync without VRR, with and without frame pacing: a frame that is decoded
// on time goes straight to the render thread and is presented at once. Any
// wait for the V-blank is the display's, after Present(), not the Pacer's.
static void testVsyncQueue()
{
    struct Display
    {
        const char* name;
        uint32_t num, den;
        // The display is slower than the 120 FPS stream: frames must wait or
        // be dropped whatever the Pacer does
        bool slowerThanStream;
    };
    const Display displays[] = { { "120.000 Hz", 120000, 1000, false },
                                 { "119.88 Hz", 120000, 1001, true },
                                 { "144 Hz", 144000, 1000, false } };
    const auto onTime = exact120Arrivals(0);
    const auto jittered = jittered120Arrivals();
    for (const Display& d : displays) {
        for (bool pacing : { false, true }) {
            char name[96];
            std::snprintf(name, sizeof(name), "V-Sync%s, 120 FPS on %s",
                          pacing ? " + frame pacing" : "", d.name);
            const VsyncLoopResult r = worstVsyncLoop(onTime, d.num, d.den, pacing);
            printVsyncLoop(name, r);
            std::snprintf(name, sizeof(name), "V-Sync%s, 120 FPS on %s, jitter",
                          pacing ? " + frame pacing" : "", d.name);
            printVsyncLoop(name, worstVsyncLoop(jittered, d.num, d.den, pacing));
            if (!d.slowerThanStream) {
                CHECK(r.dropPercent == 0);
                CHECK(r.p99QueueUs <= 500);
                CHECK(r.p99PacerHoldUs == 0);
            }
        }
    }
}

// VRR, presenting on arrival and Smooth, in the same configurations
static void testVrrQueueReport()
{
    const auto onTime = exact120Arrivals(0);
    const auto jittered = jittered120Arrivals();
    for (bool smooth : { false, true }) {
        for (int i = 0; i < 3; i++) {
            const uint32_t num[] = { 120000, 120000, 144000 };
            const uint32_t den[] = { 1000, 1001, 1000 };
            const int hz[] = { 120, 120, 144 };
            const char* names[] = { "120.000 Hz", "119.88 Hz", "144 Hz" };
            char name[96];
            std::snprintf(name, sizeof(name), "VRR%s, 120 FPS on %s", smooth ? " Smooth" : "",
                          names[i]);
            printLoop(name, simulateVrrLoop(onTime, spinSleep, smooth, hz[i], num[i], den[i]));
            std::snprintf(name, sizeof(name), "VRR%s, 120 FPS on %s, jitter",
                          smooth ? " Smooth" : "", names[i]);
            printLoop(name, simulateVrrLoop(jittered, spinSleep, smooth, hz[i], num[i], den[i]));
        }
    }
}

// A host capturing at fps (may be fractional, e.g. 119.88) with RTP timestamps
// on the 90 kHz clock, frames arriving 1.5 ms after capture
static std::vector<Arrival> pacedArrivals(double fps, int frames)
{
    std::vector<Arrival> out;
    for (int i = 1; i <= frames; i++) {
        const uint64_t ticks = 90000 + static_cast<uint64_t>(std::llround(i * 90000.0 / fps));
        const uint64_t sourceUs = ticks * 100 / 9;
        out.push_back({ sourceUs, sourceUs + 1500 });
    }
    return out;
}

// A 119.88 Hz (120000/1001) panel can't show a 120 FPS stream: every mode
// waits up to a refresh or drops a frame every ~8 s. When the user picked the
// panel's rounded rate, ask the host for the exact rate (clientRefreshRateX100,
// which Sunshine uses as a fractional capture and encode rate).
static void testFractionalStreamRate()
{
    using FramePacing::clientRefreshRateX100;
    CHECK(clientRefreshRateX100(120, 120000, 1001) == 11988);
    CHECK(clientRefreshRateX100(60, 60000, 1001) == 5994);
    CHECK(clientRefreshRateX100(144, 143981, 1000) == 14398);
    // An integer refresh rate, or an FPS the user chose that isn't the
    // panel's: nothing to send
    CHECK(clientRefreshRateX100(120, 120000, 1000) == 0);
    CHECK(clientRefreshRateX100(60, 120000, 1001) == 0);
    CHECK(clientRefreshRateX100(120, 0, 0) == 0);

    const int x100 = clientRefreshRateX100(120, 120000, 1001);
    const auto arrivals = pacedArrivals(x100 != 0 ? x100 / 100.0 : 120.0, 3600);
    for (bool pacing : { false, true }) {
        const VsyncLoopResult r = worstVsyncLoop(arrivals, 120000, 1001, pacing);
        printVsyncLoop(pacing ? "V-Sync + pacing, requested rate on 119.88 Hz"
                              : "V-Sync, requested rate on 119.88 Hz",
                       r);
        CHECK(r.dropPercent == 0);
        CHECK(r.p99QueueUs <= 500);
    }
    const VrrLoopResult vrr = simulateVrrLoop(arrivals, spinSleep, false, 120, 120000, 1001);
    printLoop("VRR, requested rate on 119.88 Hz", vrr);
    CHECK(vrr.dropPercent == 0);
    CHECK(vrr.p99AgeUs <= 500);
}

static void testVrrStreamFps()
{
    using namespace VrrPolicy;

    // At or above the display's maximum refresh rate, VRR has no room to absorb
    // arrival jitter, so the host is asked for r - r^2/3600 instead
    CHECK(vrrStreamFps(180, 180, true, false) == 171);
    CHECK(vrrStreamFps(240, 180, true, false) == 171);
    CHECK(vrrStreamFps(144, 144, true, false) == 138);
    CHECK(vrrStreamFps(60, 60, true, false) == 59);
    CHECK(vrrStreamFps(120, 120, true, false) == 116);

    // Below the maximum the stream already has headroom
    CHECK(vrrStreamFps(179, 180, true, false) == 179);
    CHECK(vrrStreamFps(170, 180, true, false) == 170);
    CHECK(vrrStreamFps(120, 180, true, false) == 120);

    // The native-rate override, VRR off, and an unknown display keep the request
    CHECK(vrrStreamFps(180, 180, true, true) == 180);
    CHECK(vrrStreamFps(180, 180, false, false) == 180);
    CHECK(vrrStreamFps(180, 0, true, false) == 180);
    CHECK(vrrStreamFps(180, 1, true, false) == 180);
    CHECK(vrrStreamFps(0, 180, true, false) == 0);
}

int main()
{
    testMinInterval();
    testRefreshInterval();
    testDisplayMode();
    testVrrStreamFps();
    testDecide();
    testLimiter();
    testSourceCadence();
    testFrameStale();
    testRefreshRateLoop();
    testLimiterLoop();
    testPlayoutClock();
    testSmoothLoop();
    testSmoothExactRefresh();
    testVsyncQueue();
    testVrrQueueReport();
    testFractionalStreamRate();

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d VRR policy check(s) failed\n", s_Failures);
        return 1;
    }
    std::puts("VRR policy tests passed");
    return 0;
}
