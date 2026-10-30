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

// Whether to tell the host at launch/resume that this session presents in VRR
// mode. Decided before the stream window exists, so in.displayHz may be 0; the
// VRR decision does not depend on it.
constexpr bool requestHostVrr(const SyncInput& in)
{
    return decide(in).enableVrr;
}

// Launch/resume query parameters for requestHostVrr(). Vibeshine and Vibepollo
// read clientVrrRequested: they lower WGC capture buffering, hold the virtual
// display at 1000 Hz so RTP timestamps are accurate to within 1 ms, and refine
// those timestamps. Other hosts ignore unknown launch parameters.
constexpr const char* hostVrrQueryParameters(bool requestVrr)
{
    return requestVrr ? "&clientVrrRequested=1" : "";
}

// How many queued frames to drop before presenting. While the present-rate cap
// held a frame back, newer frames may have arrived; only the newest is worth
// presenting, so the rest are dropped instead of building up a backlog.
constexpr int staleQueuedFrames(int queuedFrames, bool waitedForSlot)
{
    return waitedForSlot && queuedFrames > 1 ? queuedFrames - 1 : 0;
}

// Spaces presents to at most capFps per second, and never closer together
// than one refresh of a displayHz display. Each present gets a slot on a fixed
// schedule one interval apart, so a present that goes out a little late does
// not push back the ones after it, as long as the next one still comes a full
// refresh after it. A frame that is ready after its slot is presented at once
// and the schedule restarts from it, so slow periods never build up credit
// for a later burst.
class FrameLimiter
{
public:
    explicit FrameLimiter(int capFps = 0, int displayHz = 0)
        : m_MinIntervalUs(minPresentIntervalUs(capFps)),
          m_MinGapUs(minPresentIntervalUs(displayHz) < m_MinIntervalUs
                         ? minPresentIntervalUs(displayHz)
                         : m_MinIntervalUs)
    {
    }

    bool isEnabled() const { return m_MinIntervalUs != 0; }

    uint64_t minIntervalUs() const { return m_MinIntervalUs; }

    // The earliest time a frame that is ready at nowUs may be presented
    uint64_t presentSlotUs(uint64_t nowUs) const
    {
        if (m_MinIntervalUs == 0 || !m_HasNextSlot || nowUs >= m_NextSlotUs) {
            return nowUs;
        }
        if (m_NextSlotUs - nowUs > m_MinIntervalUs) {
            // Clock went backwards; never block on it
            return nowUs;
        }
        return m_NextSlotUs;
    }

    // How long to wait before presenting at nowUs (0 = present now)
    uint64_t waitUs(uint64_t nowUs) const { return presentSlotUs(nowUs) - nowUs; }

    // Records a present in slotUs (from presentSlotUs()) that actually went
    // out at nowUs. The next slot is one interval after slotUs, unless the
    // present was a whole interval late (a stall), then one interval after
    // nowUs. Either way it is at least one display refresh after nowUs.
    void onPresented(uint64_t slotUs, uint64_t nowUs)
    {
        const uint64_t base = nowUs > slotUs && nowUs - slotUs >= m_MinIntervalUs ? nowUs : slotUs;
        const uint64_t earliest = nowUs + m_MinGapUs;
        m_NextSlotUs = base + m_MinIntervalUs > earliest ? base + m_MinIntervalUs : earliest;
        m_HasNextSlot = true;
    }

    // Records a present at nowUs without a slot: the next one may go out one
    // interval later, so any lateness carries over
    void onPresented(uint64_t nowUs) { onPresented(nowUs, nowUs); }

private:
    uint64_t m_MinIntervalUs;
    uint64_t m_MinGapUs;
    uint64_t m_NextSlotUs = 0;
    bool m_HasNextSlot = false;
};

} // namespace VrrPolicy
