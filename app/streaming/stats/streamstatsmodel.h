#pragma once

#include "statsmath.h"
#include "statssnapshot.h"

#include <atomic>
#include <cstdint>
#include <mutex>

// Collects stream statistics from the video pipeline and turns them into a
// typed StatsSnapshot for overlays, logs and session summaries.
//
// Producers (all thread safe, cheap enough to call per frame):
//   - the decoder reports each frame's arrival (recordFrameArrival) and, about
//     once a second, its counter windows (publishWindow);
//   - Pacer reports each present (recordFramePresented).
// Consumers call snapshot() whenever they want fresh numbers, and can poll
// sequence() to tell whether a new window has been published since.
//
// All times are on the LiGetMicroseconds() clock. The model never calls it
// itself; callers pass the time in, which keeps it testable.
class StreamStatsModel
{
public:
    // Present timestamps kept for recent frame time percentiles
    static constexpr size_t PresentRingCapacity = 4096;
    // Recent frame time percentiles cover this much time
    static constexpr uint64_t LowFpsWindowUs = 10 * 1000000ULL;
    static constexpr uint64_t ReceivedVideoRateMaxAgeUs = 3 * 1000000ULL;
    // Number of frame times in StatsSnapshot::frameTimeHistoryMs
    static constexpr size_t FrameTimeHistoryLength = 240;
    // A present interval longer than this is a discontinuity, not a frame
    static constexpr uint64_t MaxFrameIntervalUs = 1000000;

    StreamStatsModel();

    StreamStatsModel(const StreamStatsModel&) = delete;
    StreamStatsModel& operator=(const StreamStatsModel&) = delete;

    // Decoder thread: one call per received frame
    void recordFrameArrival(uint64_t receiveTimeUs, uint32_t rtpTimestamp);

    // Render thread: one call per presented frame
    void recordFramePresented(uint64_t presentTimeUs);

    // Decoder thread: publishes a stats window. recent is what to display
    // (the decoder uses its last two windows for stability); completed holds
    // the counters of the window that just closed, which do not overlap with
    // earlier calls and feed the session totals. Only completed's frame
    // counters are read.
    void publishWindow(const StatsWindow& recent, const StatsWindow& completed, uint64_t nowUs);

    // Any thread
    StatsSnapshot snapshot(uint64_t nowUs) const;
    static bool hasFreshReceivedVideoRate(const StatsSnapshot& snapshot, uint64_t nowUs);
    uint64_t sequence() const { return m_Sequence.load(std::memory_order_acquire); }

    // Forgets everything, e.g. at the start of a new stream
    void reset();

private:
    mutable std::mutex m_Lock;

    StatsWindow m_Recent;
    uint64_t m_RecentTimestampUs = 0;
    uint64_t m_FirstArrivalUs = 0;

    StatsMath::JitterEstimator m_Jitter;
    StatsMath::PresentTimeRing m_Presents;
    StatsMath::FrameTimeHistogram m_SessionFrameTimes;
    uint64_t m_LastPresentUs = 0;

    uint64_t m_SessionFramesTotal = 0;
    uint64_t m_SessionFramesReceived = 0;
    uint64_t m_SessionFramesDecoded = 0;
    uint64_t m_SessionFramesRendered = 0;
    uint64_t m_SessionFramesNetworkDropped = 0;
    uint64_t m_SessionFramesPacerDropped = 0;
    uint64_t m_SessionFramesPacerEligible = 0;

    std::atomic<uint64_t> m_Sequence{ 0 };
};
