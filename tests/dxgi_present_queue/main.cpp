#include "streaming/video/ffmpeg-renderers/d3d11presentpolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// Frames decoded at fps with 0.2-1.5 ms of network jitter, in order
static std::vector<uint64_t> arrivals(double fps, uint64_t durationUs)
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
    std::vector<uint64_t> out;
    for (double t = 100000; t < durationUs; t += 1e6 / fps) {
        uint64_t a = static_cast<uint64_t>(t + std::fabs(normal()) * 700);
        if (!out.empty()) {
            a = std::max(a, out.back() + 30);
        }
        out.push_back(a);
    }
    return out;
}

struct Result
{
    double renderedFps;
    double dropPercent;
    double pacerQueueMs;   // decode to render start (the stats overlay's queue)
    double presentCallMs;  // time blocked in Present()
    double displayDelayMs; // decode to the refresh that shows the frame
};

// The D3D11 render thread with V-Sync off, in a window DWM composes at a
// fixed refresh: Present() hands the frame to DXGI, which holds up to
// maxLatency of them; DWM takes the oldest at each refresh. A Present() with
// the queue full blocks until a refresh frees a slot. Mirrors the Pacer's
// render queue (3 frames, oldest dropped) and renderFrame()'s catch-up drops.
static Result simulate(const std::vector<uint64_t>& in, uint64_t refreshUs, unsigned int maxLatency,
                       bool waitBeforeLatch)
{
    const uint64_t renderUs = 300;
    std::deque<size_t> pacerQueue;
    std::deque<size_t> dxgiQueue;
    std::deque<int> history;
    size_t next = 0;
    int drops = 0, rendered = 0, shown = 0;
    double queueSum = 0, presentSum = 0, displaySum = 0;
    uint64_t nextRefreshUs = refreshUs;
    uint64_t now = in.front();

    // Process refreshes and arrivals up to untilUs, in time order
    auto advance = [&](uint64_t untilUs) {
        for (;;) {
            const uint64_t arrival = next < in.size() ? in[next] : UINT64_MAX;
            if (std::min(arrival, nextRefreshUs) > untilUs) {
                break;
            }
            if (nextRefreshUs <= arrival) {
                if (!dxgiQueue.empty()) {
                    displaySum += (nextRefreshUs - in[dxgiQueue.front()]) / 1000.0;
                    shown++;
                    dxgiQueue.pop_front();
                }
                nextRefreshUs += refreshUs;
            } else {
                if (pacerQueue.size() == 3) {
                    pacerQueue.pop_front();
                    drops++;
                }
                pacerQueue.push_back(next++);
            }
        }
        now = std::max(now, untilUs);
    };
    auto untilSlotFree = [&]() {
        while (dxgiQueue.size() >= maxLatency) {
            advance(nextRefreshUs);
        }
    };

    while (next < in.size() || !pacerQueue.empty()) {
        if (waitBeforeLatch) {
            // waitToRender(): the frame latency waitable object
            untilSlotFree();
        }
        while (pacerQueue.empty() && next < in.size()) {
            advance(in[next]);
        }
        if (pacerQueue.empty()) {
            break;
        }
        if (waitBeforeLatch) {
            while (pacerQueue.size() > 1) {
                pacerQueue.pop_front();
                drops++;
            }
        }
        const size_t frame = pacerQueue.front();
        pacerQueue.pop_front();
        queueSum += (now - in[frame]) / 1000.0;
        advance(now + renderUs);

        const uint64_t presentStart = now;
        untilSlotFree();
        presentSum += (now - presentStart) / 1000.0;
        dxgiQueue.push_back(frame);
        rendered++;

        // renderFrame(): drop frames if too many stay queued for a while
        int target = 0;
        for (int h : history) {
            if (h == 0) {
                target = 2;
                break;
            }
        }
        if (history.size() == 90) {
            history.pop_front();
        }
        history.push_back(static_cast<int>(pacerQueue.size()));
        while (static_cast<int>(pacerQueue.size()) > target) {
            pacerQueue.pop_front();
            drops++;
        }
    }
    const double seconds = (in.back() - in.front()) / 1e6;
    return { rendered / seconds, 100.0 * drops / in.size(), queueSum / rendered,
             presentSum / rendered, displaySum / std::max(1, shown) };
}

static void print(const char* name, const Result& r)
{
    std::printf("%-40s %6.1f FPS rendered, %4.1f%% dropped, queue %5.2f ms, Present %5.2f ms, "
                "decode to display %5.2f ms\n",
                name, r.renderedFps, r.dropPercent, r.pacerQueueMs, r.presentCallMs,
                r.displayDelayMs);
}

int main()
{
    using namespace D3D11PresentPolicy;
    const uint64_t durationUs = 10000000;
    const uint64_t refresh180 = 5556;
    // The field case (r33, V-Sync, frame pacing and VRR off): Present() took
    // ~5.5 ms and ~170 of ~179 frames per second were rendered, so DWM drained
    // the composed window's frames at ~170 per second
    const uint64_t fieldDrainUs = 5880;

    const auto stream180 = arrivals(180, durationUs);
    const Result legacy = simulate(stream180, fieldDrainUs, 3, false);
    const Result current = simulate(stream180, fieldDrainUs, MaxFrameLatency, WaitBeforeLatch);
    print("180 FPS, DWM drains 170/s, Present(0) x3", legacy);
    print("180 FPS, DWM drains 170/s, renderer", current);

    // The model reproduces the field run with the old present path
    CHECK(legacy.presentCallMs > 4.0);
    CHECK(legacy.renderedFps > 165 && legacy.renderedFps < 175);
    CHECK(legacy.pacerQueueMs > 4.0);

    // The renderer never blocks in Present(), and shows each frame within
    // about a refresh of decoding. While DWM is the bottleneck the newest
    // frame still waits for the free slot, on average half a refresh.
    CHECK(current.presentCallMs < 0.5);
    CHECK(current.pacerQueueMs < fieldDrainUs / 2 / 1000.0);
    CHECK(current.displayDelayMs < fieldDrainUs * 1.5 / 1000.0);
    CHECK(current.displayDelayMs * 2 < legacy.displayDelayMs);
    CHECK(current.renderedFps >= legacy.renderedFps - 5);

    // DWM keeping up (180 FPS at 180 Hz) or a slower stream: nothing to wait
    // for, so the renderer adds no drops or delay
    const Result matched = simulate(stream180, refresh180, MaxFrameLatency, WaitBeforeLatch);
    print("180 FPS, DWM drains 180/s, renderer", matched);
    CHECK(matched.dropPercent < 1.0);
    CHECK(matched.displayDelayMs < refresh180 / 1000.0 + 0.5);
    const auto stream120 = arrivals(120, durationUs);
    const Result slow = simulate(stream120, refresh180, MaxFrameLatency, WaitBeforeLatch);
    print("120 FPS, DWM drains 180/s, renderer", slow);
    CHECK(slow.dropPercent < 0.5);
    CHECK(slow.pacerQueueMs < 0.5);
    CHECK(slow.displayDelayMs < refresh180 / 1000.0 + 0.5);

    if (s_Failures != 0) {
        std::fprintf(stderr, "%d DXGI present queue check(s) failed\n", s_Failures);
        return 1;
    }
    std::puts("DXGI present queue tests passed");
    return 0;
}
