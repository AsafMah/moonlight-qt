#pragma once

#include <cstdint>

// Variable refresh rate (G-Sync / FreeSync) presentation policy.
//
// In VRR mode every frame is presented as soon as it is decoded, without
// V-Sync waits or frame pacing, so a VRR display refreshes when each frame
// arrives. The only wait is a present-rate cap a little below the display's
// refresh rate: frames presented faster than the panel's maximum leave the VRR
// range and tear (or get held for V-Sync by the driver).
//
// Everything here is pure so it can be unit tested.
namespace VrrPolicy {

// The highest present rate that stays safely inside the VRR range of a
// display running at displayHz: refresh - refresh^2 / 3600, rounded away from
// the maximum, and at least 3 below it (60 -> 57, 120 -> 116, 144 -> 138,
// 240 -> 224). Returns 0 (no cap) for an unknown refresh rate.
constexpr int frameCapFps(int displayHz)
{
    if (displayHz <= 3) {
        return 0;
    }
    const int squaredMargin = (displayHz * displayHz + 3599) / 3600;
    const int margin = squaredMargin > 3 ? squaredMargin : 3;
    return displayHz - margin;
}

// The shortest time between two presents for a cap, rounded up so the cap is
// never exceeded. 0 means no cap.
constexpr uint64_t minPresentIntervalUs(int capFps)
{
    return capFps > 0
               ? (1000000u + static_cast<uint64_t>(capFps) - 1u) / static_cast<uint64_t>(capFps)
               : 0;
}

// Whether a full-screen display mode suits the stream. Without VRR the stream
// FPS must evenly divide the refresh rate; with VRR the display follows the
// stream, so any refresh rate works and the highest one gives the widest range.
constexpr bool displayModeSuitsStream(int refreshHz, int streamFps, bool enableVrr)
{
    if (enableVrr) {
        return refreshHz > 0;
    }
    return streamFps > 0 && refreshHz % streamFps == 0;
}

struct SyncInput
{
    bool enableVsync;
    bool framePacing;
    bool enableVrr;
    int streamFps;
    int displayHz;
};

struct SyncDecision
{
    bool enableVsync;
    bool enableFramePacing;
    bool enableVrr;
    // Present-rate cap in VRR mode, 0 when there is none
    int presentCapFps;
    // True when the stream's frame rate is above the cap, so some frames will
    // be dropped to stay inside the VRR range
    bool streamAboveVrrRange;
};

// Turns the saved preferences into the effective presentation settings for
// one decoder instance. With VRR off this is exactly the old behaviour:
// V-Sync is forced off when the stream exceeds the refresh rate (plus some
// slack), and frame pacing needs V-Sync.
constexpr SyncDecision decide(const SyncInput& in)
{
    if (in.enableVrr) {
        const int cap = frameCapFps(in.displayHz);
        return SyncDecision{ false, false, true, cap, cap > 0 && in.streamFps > cap };
    }

    const bool vsync = in.enableVsync && !(in.displayHz + 5 < in.streamFps);
    return SyncDecision{ vsync, vsync && in.framePacing, false, 0, false };
}

// Spaces presents at least minPresentIntervalUs(capFps) apart. Frames are
// never reordered or dropped here; the caller only waits before presenting.
class FrameLimiter
{
public:
    explicit FrameLimiter(int capFps = 0) : m_MinIntervalUs(minPresentIntervalUs(capFps)) {}

    bool isEnabled() const { return m_MinIntervalUs != 0; }

    uint64_t minIntervalUs() const { return m_MinIntervalUs; }

    // How long to wait before presenting at nowUs (0 = present now)
    uint64_t waitUs(uint64_t nowUs) const
    {
        if (m_MinIntervalUs == 0 || !m_HasLastPresent) {
            return 0;
        }
        if (nowUs < m_LastPresentUs) {
            // Clock went backwards; never block on it
            return 0;
        }
        const uint64_t elapsed = nowUs - m_LastPresentUs;
        return elapsed >= m_MinIntervalUs ? 0 : m_MinIntervalUs - elapsed;
    }

    void onPresented(uint64_t nowUs)
    {
        m_LastPresentUs = nowUs;
        m_HasLastPresent = true;
    }

private:
    uint64_t m_MinIntervalUs;
    uint64_t m_LastPresentUs = 0;
    bool m_HasLastPresent = false;
};

} // namespace VrrPolicy
