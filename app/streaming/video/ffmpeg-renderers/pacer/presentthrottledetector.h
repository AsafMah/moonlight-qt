#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Detects a present rate held below the stream rate by something outside the
// pacer: a driver frame limiter (NVIDIA Max Frame Rate, or the automatic cap
// that Low Latency Mode Ultra / Reflex applies under G-Sync, r - r^2/3600 FPS)
// or an external limiter. Signature over one 2 s window: Present() itself
// blocks (p50 above 1 ms), the presents are spaced further apart than the
// expected rate allows, and frames arrive faster than they are presented, so
// the pacer is discarding frames rather than starving. A fixed-refresh V-Sync
// panel also blocks in Present, but at exactly one refresh, which is the
// expected rate here, so it does not trigger this; neither does a game that
// sends fewer frames than the cap allows.
class PresentThrottleDetector
{
public:
    static constexpr uint64_t WindowUs = 2000000;
    // A gap this long (stream paused, window moved) starts a new window
    static constexpr uint64_t GapResetUs = 100000;
    static constexpr uint64_t BlockingPresentUs = 1000;
    static constexpr unsigned int MinIntervals = 30;
    // Intervals must exceed the expected one by more than 1%: NVIDIA's
    // automatic cap is at least 1.6% below the refresh rate from 60 Hz up
    static constexpr unsigned int SlowIntervalPercent = 101;
    // Share of the frames received in the window that were never presented
    static constexpr unsigned int MinUnpresentedPerMille = 5;

    struct Report
    {
        // Achieved present rate: 1 s / median present interval, rounded
        unsigned int throttledFps = 0;
        unsigned int streamFps = 0;
        // Highest rate presents could reach: the stream rate, or the
        // display's refresh rate if lower
        unsigned int expectedFps = 0;
        uint64_t medianIntervalUs = 0;
        uint64_t presentP50Us = 0;
        uint64_t prePresentP50Us = 0;
        // Frames handed to the pacer and frames presented over the window
        unsigned int receivedFrames = 0;
        unsigned int presentedFrames = 0;
    };

    PresentThrottleDetector(unsigned int streamFps, unsigned int displayHz)
    {
        m_Report.streamFps = streamFps;
        m_Report.expectedFps = displayHz > 0 && displayHz < streamFps ? displayHz : streamFps;
        m_ExpectedIntervalUs = m_Report.expectedFps > 0 ? 1000000ULL / m_Report.expectedFps : 0;
    }

    // Call once per successful present with the cumulative number of frames
    // handed to the pacer so far. Returns true once, on the present that
    // completes the first window showing a throttle.
    bool observe(uint64_t presentEndUs, uint64_t prePresentUs, uint64_t presentUs,
                 uint64_t framesReceived)
    {
        if (m_Triggered || m_ExpectedIntervalUs == 0) {
            return false;
        }

        if (m_LastEndUs == 0 || presentEndUs <= m_LastEndUs ||
            presentEndUs - m_LastEndUs > GapResetUs) {
            startWindow(presentEndUs, framesReceived);
        } else {
            m_IntervalsUs.push_back(presentEndUs - m_LastEndUs);
        }
        m_LastEndUs = presentEndUs;
        m_FramesReceived = framesReceived;
        m_PresentUs.push_back(presentUs);
        m_PrePresentUs.push_back(prePresentUs);

        if (presentEndUs - m_WindowStartUs < WindowUs) {
            return false;
        }
        m_Triggered = evaluate();
        startWindow(presentEndUs, framesReceived);
        return m_Triggered;
    }

    // Filled when observe() returns true
    const Report& report() const { return m_Report; }

private:
    static uint64_t median(std::vector<uint64_t>& values)
    {
        auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    }

    void startWindow(uint64_t nowUs, uint64_t framesReceived)
    {
        m_WindowStartUs = nowUs;
        m_WindowStartReceived = framesReceived;
        m_IntervalsUs.clear();
        m_PresentUs.clear();
        m_PrePresentUs.clear();
    }

    bool evaluate()
    {
        if (m_IntervalsUs.size() < MinIntervals) {
            return false;
        }
        // Presents after the window's first one, against the frames received
        // over the same span
        const uint64_t presented = m_IntervalsUs.size();
        const uint64_t received = m_FramesReceived - m_WindowStartReceived;
        if (presented >= received ||
            (received - presented) * 1000 < received * MinUnpresentedPerMille) {
            return false;
        }
        const uint64_t presentP50Us = median(m_PresentUs);
        if (presentP50Us <= BlockingPresentUs) {
            return false;
        }
        const uint64_t medianIntervalUs = median(m_IntervalsUs);
        if (medianIntervalUs * 100 <= m_ExpectedIntervalUs * SlowIntervalPercent) {
            return false;
        }

        m_Report.throttledFps =
            static_cast<unsigned int>((1000000ULL + medianIntervalUs / 2) / medianIntervalUs);
        m_Report.medianIntervalUs = medianIntervalUs;
        m_Report.presentP50Us = presentP50Us;
        m_Report.prePresentP50Us = median(m_PrePresentUs);
        m_Report.receivedFrames = static_cast<unsigned int>(received);
        m_Report.presentedFrames = static_cast<unsigned int>(presented);
        return true;
    }

    Report m_Report;
    uint64_t m_ExpectedIntervalUs = 0;
    uint64_t m_WindowStartUs = 0;
    uint64_t m_WindowStartReceived = 0;
    uint64_t m_LastEndUs = 0;
    uint64_t m_FramesReceived = 0;
    bool m_Triggered = false;
    std::vector<uint64_t> m_IntervalsUs;
    std::vector<uint64_t> m_PresentUs;
    std::vector<uint64_t> m_PrePresentUs;
};
