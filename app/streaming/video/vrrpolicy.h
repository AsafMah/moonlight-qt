#pragma once

#include <algorithm>
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
    // The Nonary VRR pacer (ported from Nonary/moonlight-qt) is selected
    bool nonaryVrr = false;
};

struct SyncDecision
{
    bool enableVsync;
    bool enableFramePacing;
    bool enableVrr;
    // Present-rate cap in VRR mode, 0 when there is none. With the Nonary
    // pacer this only applies if it falls back to the fork limiter.
    int presentCapFps;
    // True when the stream's frame rate is above the cap, so some frames will
    // be dropped to stay inside the VRR range
    bool streamAboveVrrRange;
    // The Nonary pacer runs this decoder: V-Sync stays on (it presents
    // tearing or synchronized per frame) and frame pacing stays off
    bool nonaryVrr = false;
};

// Turns the saved preferences into the effective presentation settings for
// one decoder instance. With VRR off this is exactly the old behaviour:
// V-Sync is forced off when the stream exceeds the refresh rate (plus some
// slack), and frame pacing needs V-Sync.
constexpr SyncDecision decide(const SyncInput& in)
{
    if (in.enableVrr) {
        const int cap = frameCapFps(in.displayHz);
        if (in.nonaryVrr) {
            // Nonary drops only above the native refresh rate
            return SyncDecision{
                true, false, true, cap, in.displayHz > 0 && in.streamFps > in.displayHz, true
            };
        }
        return SyncDecision{ false, false, true, cap, cap > 0 && in.streamFps > cap };
    }

    const bool vsync = in.enableVsync && !(in.displayHz + 5 < in.streamFps);
    return SyncDecision{ vsync, vsync && in.framePacing, false, 0, false };
}

// Measures the content's frame period from sender (RTP) timestamps, so bursty
// network delivery doesn't look like a faster stream. 0 until known.
class SourceCadence
{
public:
    void addFrame(uint64_t sourceUs)
    {
        if (m_Count > 0) {
            const uint64_t newest = m_SourceUs[(m_Next + Window - 1) % Window];
            // Restart on a timestamp that goes backwards (wrap) or jumps
            if (sourceUs <= newest || sourceUs - newest > 1000000) {
                m_Count = 0;
            }
        }
        m_SourceUs[m_Next] = sourceUs;
        m_Next = (m_Next + 1) % Window;
        if (m_Count < Window) {
            m_Count++;
        }
    }

    uint64_t periodUs() const
    {
        if (m_Count < 2) {
            return 0;
        }
        const uint64_t newest = m_SourceUs[(m_Next + Window - 1) % Window];
        const uint64_t oldest = m_SourceUs[(m_Next + Window - m_Count) % Window];
        return (newest - oldest) / (m_Count - 1);
    }

private:
    static constexpr int Window = 32;
    uint64_t m_SourceUs[Window] = {};
    int m_Next = 0;
    int m_Count = 0;
};

// Whether to drop the oldest queued frame, ageUs old, for a newer one that is
// already queued (queuedFrames > 1), after the present-rate cap held it back
// for waitedUs.
//
// When the content runs faster than the cap, frames held back by it are
// always superseded, so only the newest is presented. Below the cap a frame
// is only held back by a burst of arrivals, such as two frames delivered
// together. It is still presented unless it is more than two source periods
// old, or one period plus the time the cap already held it (after the
// stale-frame policy of Nonary's VRR pacer, Nonary/moonlight-qt@c822745,
// vrrframedroppolicy.h, GPLv3), or presenting the whole queue at the cap would
// leave the newest frame more than a source period behind (a burst after a
// network stall), or the content is so close to the cap that the backlog
// can't catch up within 8 frames. An unknown cadence counts as at the cap.
constexpr bool isFrameStale(uint64_t ageUs, uint64_t waitedUs, int queuedFrames,
                            uint64_t sourcePeriodUs, uint64_t minIntervalUs)
{
    const uint64_t periodUs = sourcePeriodUs != 0 ? sourcePeriodUs : minIntervalUs;
    if (periodUs < minIntervalUs) {
        return waitedUs > 0;
    }
    const uint64_t horizonUs =
        2 * periodUs > periodUs + waitedUs ? 2 * periodUs : periodUs + waitedUs;
    const uint64_t backlogUs =
        queuedFrames > 1 ? static_cast<uint64_t>(queuedFrames - 1) * minIntervalUs : 0;
    // Each frame after a burst gains (period - cap interval) on the schedule.
    // Just under the cap that is so little that a kept frame leaves a standing
    // queue, so keep it only if the backlog drains within 8 frames.
    const bool drainsQuickly = backlogUs <= 8 * (periodUs - minIntervalUs);
    return ageUs > horizonUs || backlogUs > periodUs || !drainsQuickly;
}

// Smooth mode: instead of presenting each frame on arrival, present it at
// its sender (RTP) timestamp plus a playout delay, so network jitter doesn't
// reach the screen. The delay is learned: the smallest arrival - source
// offset over the window is the fastest path (it also absorbs the clock
// difference between host and client), and the delay covers the 95th
// percentile of how much later than that frames arrive, plus a margin.
class PlayoutClock
{
public:
    // A frame captured at sourceUs (sender clock) was decoded at arrivalUs
    void observe(uint64_t sourceUs, uint64_t arrivalUs)
    {
        // Restart on a sender timestamp that goes backwards (wrap) or jumps
        if (m_Count > 0 && (sourceUs <= m_LastSourceUs || sourceUs - m_LastSourceUs > 1000000)) {
            m_Count = 0;
        }
        m_LastSourceUs = sourceUs;
        m_OffsetUs[m_Next] = static_cast<int64_t>(arrivalUs) - static_cast<int64_t>(sourceUs);
        m_Next = (m_Next + 1) % Window;
        if (m_Count < Window) {
            m_Count++;
        }

        int64_t lateness[Window];
        int64_t fastest = m_OffsetUs[(m_Next + Window - 1) % Window];
        for (int i = 1; i < m_Count; i++) {
            fastest = (std::min)(fastest, m_OffsetUs[(m_Next + Window - 1 - i) % Window]);
        }
        for (int i = 0; i < m_Count; i++) {
            lateness[i] = m_OffsetUs[(m_Next + Window - 1 - i) % Window] - fastest;
        }
        const int p95 = m_Count * 95 / 100;
        std::nth_element(lateness, lateness + p95, lateness + m_Count);
        m_FastestOffsetUs = fastest;
        m_DelayUs =
            (std::min<uint64_t>)(static_cast<uint64_t>(lateness[p95]) + MarginUs, MaxDelayUs);
    }

    // When to present the frame captured at sourceUs; 0 while unknown
    uint64_t targetUs(uint64_t sourceUs) const
    {
        if (m_Count < MinSamples) {
            return 0;
        }
        const int64_t target =
            static_cast<int64_t>(sourceUs) + m_FastestOffsetUs + static_cast<int64_t>(m_DelayUs);
        return target > 0 ? static_cast<uint64_t>(target) : 0;
    }

    uint64_t delayUs() const { return m_Count < MinSamples ? 0 : m_DelayUs; }

private:
    // About 1.4 s at 180 FPS
    static constexpr int Window = 256;
    static constexpr int MinSamples = 16;
    static constexpr uint64_t MarginUs = 500;
    static constexpr uint64_t MaxDelayUs = 20000;
    int64_t m_OffsetUs[Window] = {};
    int m_Next = 0;
    int m_Count = 0;
    uint64_t m_LastSourceUs = 0;
    int64_t m_FastestOffsetUs = 0;
    uint64_t m_DelayUs = 0;
};

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
