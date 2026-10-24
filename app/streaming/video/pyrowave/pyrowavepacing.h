#pragma once

#include <cstdint>

// Presentation policy for the PyroWave decoder, kept free of SDL, Vulkan and
// Qt so it can be unit tested (tests/pyrowave_pacing). Times are in
// microseconds on any monotonic clock.
namespace PyrowavePacing {

// The interval a present is judged against: the slower of the stream and the
// display. With V-Sync on a display slower than the stream, every present
// waits for the display, which no swapchain depth can fix. 0 (unknown) rates
// are ignored; with both unknown it assumes 60 Hz.
uint64_t frameIntervalUs(int streamFps, int displayFps);

// When to switch the swapchain from double to triple buffering, following
// PlVkRenderer's PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH rule: a present that takes
// more than 110% of the frame interval counts up, any other counts down, and
// half a second's worth of frames (at the stream rate) in excess switches.
// It never switches back within a renderer's life, like PlVkRenderer.
class SwapchainDepth
{
public:
    static constexpr int k_DoubleBuffered = 1;
    static constexpr int k_TripleBuffered = 2;

    void configure(int streamFps, int displayFps);

    // presentUs: swapchain acquire, rendering and submit for one frame.
    // Returns true once, for the present that should switch to
    // k_TripleBuffered.
    bool recordPresent(uint64_t presentUs);

    int depth() const { return m_Depth; }
    int delayedPresents() const { return m_DelayedPresents; }

private:
    int m_StreamFps = 60;
    uint64_t m_ThresholdUs = 18333;
    int m_DelayedPresents = 0;
    int m_Depth = k_DoubleBuffered;
};

// Frame pacing for a stream faster than the display, with V-Sync and frame
// pacing on. Each PyroWave frame is intra-coded, so frames can be skipped
// before decoding at no cost to the next one. Without this, every present
// waits in the swapchain for the display and the frames behind it are
// dropped unevenly (only when two are queued). The pacer instead presents
// display/stream of the frames, evenly spread: every second frame at 120 on
// 60 Hz, 5 of every 12 at 144 on 60 Hz.
//
// A stream at or below the display rate presents every frame. Smoothing
// network jitter by holding frames back (as Pacer can) is not done: it would
// add latency to a codec chosen for low latency.
class FramePacer
{
public:
    void configure(bool enabled, int streamFps, int displayFps);

    // True when the pacer drops some frames: enabled and the display is
    // known and slower than the stream.
    bool isDecimating() const { return m_Decimating; }

    // Called for every frame about to be decoded. False means skip it.
    bool shouldPresent();

private:
    bool m_Decimating = false;
    int m_StreamFps = 0;
    int m_DisplayFps = 0;
    // Display refreshes owed, in units of 1/streamFps. Stays below
    // streamFps between frames, so presents never come in a row.
    int m_Credit = 0;
};

}
