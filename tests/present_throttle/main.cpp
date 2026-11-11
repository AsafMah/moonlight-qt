#include "streaming/video/ffmpeg-renderers/pacer/presentthrottledetector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int s_Failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            s_Failures++;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

struct Rng
{
    uint64_t state;
    double uniform()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

enum class PresentModel
{
    // Present returns after presentCostUs (tearing / VRR with headroom)
    Free,
    // A driver frame limiter: Present returns no sooner than capIntervalUs after the previous one
    DriverCap,
    // Fixed refresh V-Sync: one flip per vblank, Present returns at the vblank
    FixedVsync,
};

struct SimConfig
{
    unsigned int streamFps = 180;
    unsigned int displayHz = 180;
    // Rate the host actually sends (a game below the stream rate), 0 = streamFps
    double sendFps = 0;
    // Uniform +/- network jitter on each arrival
    double jitterUs = 800;
    double prePresentUs = 250;
    // Render-loop work after Present returns, before the next frame is picked
    double loopOverheadUs = 230;
    PresentModel model = PresentModel::Free;
    double presentCostUs = 150;
    double capIntervalUs = 0;
    double vblankUs = 0;
    // Fork VRR limiter: minimum spacing between present starts (0 = none)
    double limiterUs = 0;
    double seconds = 30;
    uint64_t seed = 1;
};

struct SimResult
{
    double renderedFps = 0;
    double dropPercent = 0;
    double averageRenderUs = 0;
    double averageFrameTimeUs = 0;
    bool fired = false;
    uint64_t firedAtUs = 0;
    PresentThrottleDetector::Report report;
};

// Pacer render-thread model: newest-wins (stale frames are dropped when a newer
// one is ready), optional fork VRR limiter, then pre-Present work and Present.
SimResult simulate(const SimConfig& config)
{
    Rng rng{ config.seed };
    const double sendFps = config.sendFps > 0 ? config.sendFps : config.streamFps;
    const double periodUs = 1000000.0 / sendFps;
    const double startUs = 1000000.0;
    std::vector<double> arrivals;
    for (double t = startUs; t < startUs + config.seconds * 1000000.0; t += periodUs) {
        double arrival = t + (rng.uniform() * 2 - 1) * config.jitterUs;
        if (!arrivals.empty() && arrival <= arrivals.back()) {
            arrival = arrivals.back() + 1;
        }
        arrivals.push_back(arrival);
    }

    PresentThrottleDetector detector(config.streamFps, config.displayHz);
    SimResult result;
    size_t next = 0;
    double now = 0;
    double lastStart = -1e12;
    double lastEnd = -1e12;
    uint64_t presents = 0;
    uint64_t drops = 0;
    double totalRenderUs = 0;
    double totalFrameTimeUs = 0;
    while (next < arrivals.size()) {
        now = std::max(now, arrivals[next]);
        if (config.limiterUs > 0) {
            now = std::max(now, lastStart + config.limiterUs);
        }
        // Newest wins: everything older than the newest ready frame is stale
        while (next + 1 < arrivals.size() && arrivals[next + 1] <= now) {
            ++drops;
            ++next;
        }
        ++next;

        const double start = now;
        const double call = start + config.prePresentUs;
        double end = call + config.presentCostUs;
        if (config.model == PresentModel::DriverCap) {
            end = std::max(end, lastEnd + config.capIntervalUs);
        } else if (config.model == PresentModel::FixedVsync) {
            double vblank = std::ceil(call / config.vblankUs) * config.vblankUs;
            if (vblank < lastEnd + config.vblankUs * 0.5) {
                vblank += config.vblankUs;
            }
            end = std::max(end, vblank);
        }

        uint64_t received = next;
        while (received < arrivals.size() && arrivals[received] <= end) {
            ++received;
        }

        if (presents > 0) {
            totalFrameTimeUs += end - lastEnd;
        }
        ++presents;
        totalRenderUs += end - start;
        if (detector.observe(static_cast<uint64_t>(end), static_cast<uint64_t>(call - start),
                             static_cast<uint64_t>(end - call), received)) {
            CHECK(!result.fired);
            result.fired = true;
            result.firedAtUs = static_cast<uint64_t>(end - startUs);
            result.report = detector.report();
        }
        lastStart = start;
        lastEnd = end;
        now = end + config.loopOverheadUs;
    }

    result.renderedFps = presents / config.seconds;
    result.dropPercent = 100.0 * drops / static_cast<double>(drops + presents);
    result.averageRenderUs = totalRenderUs / presents;
    result.averageFrameTimeUs = presents > 1 ? totalFrameTimeUs / (presents - 1) : 0;
    return result;
}

void printResult(const char* name, const SimResult& r)
{
    std::printf("%-34s Rd %6.1f  drops %5.2f%%  render %5.0f us  frame %5.0f us  %s", name,
                r.renderedFps, r.dropPercent, r.averageRenderUs, r.averageFrameTimeUs,
                r.fired ? "WARN" : "quiet");
    if (r.fired) {
        std::printf(" at %.2f s: N=%u M=%u interval=%llu us Present p50=%llu us "
                    "pre-Present p50=%llu us presented %u of %u",
                    r.firedAtUs / 1e6, r.report.throttledFps, r.report.streamFps,
                    static_cast<unsigned long long>(r.report.medianIntervalUs),
                    static_cast<unsigned long long>(r.report.presentP50Us),
                    static_cast<unsigned long long>(r.report.prePresentP50Us),
                    r.report.presentedFrames, r.report.receivedFrames);
    }
    std::printf("\n");
}

// Fixture: Moonlight-1794415843 (fork VRR, tearing, exact 180/1 refresh, NVIDIA
// per-program frame cap active): Rx 180.0, Rd 170.2, drops 5.43%, render 5.64 ms,
// frame time 5.87 ms average / 5.90 ms p99, network jitter 0.42-0.48 ms.
SimConfig driverCapFixture()
{
    SimConfig config;
    config.model = PresentModel::DriverCap;
    // The fixture's measured present cadence (the 171 FPS cap plus the driver's wake-up slip)
    config.capIntervalUs = 5875;
    config.limiterUs = 1000000.0 / 180 - 50;
    return config;
}

void testDriverCapFixtureWarns()
{
    const SimResult r = simulate(driverCapFixture());
    printResult("cd28 fixture (driver cap)", r);
    CHECK(r.renderedFps > 169.0 && r.renderedFps < 171.5);
    CHECK(r.dropPercent > 4.5 && r.dropPercent < 6.5);
    CHECK(r.averageRenderUs > 5400 && r.averageRenderUs < 5900);
    CHECK(r.averageFrameTimeUs > 5840 && r.averageFrameTimeUs < 5900);
    CHECK(r.fired);
    CHECK(r.firedAtUs <= 2100000);
    CHECK(r.report.throttledFps == 170);
    CHECK(r.report.streamFps == 180);
    CHECK(r.report.presentP50Us > 5000);
    CHECK(r.report.presentedFrames * 100 < r.report.receivedFrames * 97);
}

// Fixture: Moonlight-1794416296 (VRR on, NVIDIA cap removed, 180-on-180):
// Rd 179.0, drops 0.43%, render 0.25 ms, network jitter 0.38 ms.
void testHealthyVrrIsQuiet()
{
    SimConfig config;
    config.jitterUs = 660;
    config.prePresentUs = 100;
    config.presentCostUs = 150;
    config.limiterUs = 1000000.0 / 180 - 50;
    const SimResult r = simulate(config);
    printResult("e1d1 fixture (healthy VRR)", r);
    CHECK(r.renderedFps > 177.0);
    CHECK(r.averageRenderUs < 400);
    CHECK(!r.fired);
}

// A fixed 180 Hz panel with V-Sync: Present blocks, but at exactly one refresh,
// which is the display's own rate rather than a throttle
void testFixedRefreshVsyncIsQuiet()
{
    SimConfig config;
    config.model = PresentModel::FixedVsync;
    config.vblankUs = 1000000.0 / 179.98;
    const SimResult r = simulate(config);
    printResult("fixed 180 Hz V-Sync, 180 stream", r);
    CHECK(r.averageRenderUs > 1000);
    CHECK(!r.fired);
}

void testStreamAboveFixedRefreshIsQuiet()
{
    SimConfig config;
    config.displayHz = 144;
    config.model = PresentModel::FixedVsync;
    config.vblankUs = 1000000.0 / 144;
    const SimResult r = simulate(config);
    printResult("fixed 144 Hz V-Sync, 180 stream", r);
    CHECK(r.renderedFps < 145.0);
    CHECK(!r.fired);
}

// The host sends fewer frames than the cap allows: the cap never holds a frame back
void testSlowGameUnderCapIsQuiet()
{
    SimConfig config = driverCapFixture();
    config.sendFps = 120;
    const SimResult r = simulate(config);
    printResult("120 FPS game under the cap", r);
    CHECK(r.renderedFps > 119.0);
    CHECK(!r.fired);
}

// GPU-bound rendering is slow before Present, not inside it
void testGpuBoundIsQuiet()
{
    SimConfig config;
    config.prePresentUs = 6000;
    const SimResult r = simulate(config);
    printResult("GPU-bound pre-Present", r);
    CHECK(r.renderedFps < 165.0);
    CHECK(!r.fired);
}

// NVIDIA's automatic cap at 60 Hz is 59 FPS (r - r^2/3600)
void testSixtyHzCapWarns()
{
    SimConfig config;
    config.streamFps = 60;
    config.displayHz = 60;
    config.model = PresentModel::DriverCap;
    config.capIntervalUs = 1000000.0 / 59;
    config.limiterUs = 1000000.0 / 60 - 50;
    config.jitterUs = 1500;
    const SimResult r = simulate(config);
    printResult("60 Hz, 59 FPS driver cap", r);
    CHECK(r.fired);
    CHECK(r.report.throttledFps == 59);
    CHECK(r.report.streamFps == 60);
}

void testWarnsOnceAndGapResetsWindow()
{
    PresentThrottleDetector detector(180, 180);
    uint64_t now = 1000000;
    uint64_t received = 0;
    unsigned int fired = 0;
    // 1.5 s throttled, a 150 ms gap (stream paused), 1.5 s throttled: no
    // single 2 s window is complete yet
    for (int i = 0; i < 255; i++) {
        now += 5875;
        received += i % 17 == 0 ? 2 : 1;
        fired += detector.observe(now, 250, 5400, received);
    }
    now += 150000;
    for (int i = 0; i < 255; i++) {
        now += 5875;
        received += i % 17 == 0 ? 2 : 1;
        fired += detector.observe(now, 250, 5400, received);
    }
    CHECK(fired == 0);
    for (int i = 0; i < 200; i++) {
        now += 5875;
        received += i % 17 == 0 ? 2 : 1;
        fired += detector.observe(now, 250, 5400, received);
    }
    CHECK(fired == 1);
    for (int i = 0; i < 2000; i++) {
        now += 5875;
        received += i % 17 == 0 ? 2 : 1;
        fired += detector.observe(now, 250, 5400, received);
    }
    CHECK(fired == 1);
}

// Presents blocked and spaced wider than the stream, but every received frame is shown
void testNoDiscardedFramesIsQuiet()
{
    PresentThrottleDetector detector(180, 180);
    uint64_t now = 1000000;
    bool fired = false;
    for (int i = 0; i < 2000; i++) {
        now += 5875;
        fired |= detector.observe(now, 250, 5400, static_cast<uint64_t>(i) + 1);
    }
    CHECK(!fired);
}

void testUnknownRatesAreQuiet()
{
    PresentThrottleDetector detector(0, 0);
    uint64_t now = 1000000;
    bool fired = false;
    for (int i = 0; i < 2000; i++) {
        now += 50000;
        fired |= detector.observe(now, 250, 49000, static_cast<uint64_t>(i) * 2);
    }
    CHECK(!fired);
}

} // namespace

int main()
{
    testDriverCapFixtureWarns();
    testHealthyVrrIsQuiet();
    testFixedRefreshVsyncIsQuiet();
    testStreamAboveFixedRefreshIsQuiet();
    testSlowGameUnderCapIsQuiet();
    testGpuBoundIsQuiet();
    testSixtyHzCapWarns();
    testWarnsOnceAndGapResetsWindow();
    testNoDiscardedFramesIsQuiet();
    testUnknownRatesAreQuiet();
    return s_Failures == 0 ? 0 : 1;
}
