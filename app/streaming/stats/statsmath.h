#pragma once

#include "statssnapshot.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The arithmetic behind StreamStatsModel, kept free of SDL and
// moonlight-common-c so it can be unit tested (tests/stats_model).
namespace StatsMath {

// Nearest-rank percentile of an unsorted sample set, with p in [0, 1].
// Reorders the values. Returns 0 for an empty set.
double percentile(std::vector<double>& values, double p);

// Average, maximum and percentile frame times, plus the frame rates they imply.
//
// "1% low" follows the percentile convention used by PresentMon-style tools:
// the frame rate equivalent of the 99th percentile frame time, so 1% of frames
// took longer than 1000 / onePercentLowFps milliseconds. "0.1% low" uses the
// 99.9th percentile. With fewer than 100 (or 1000) samples these degrade to
// the slowest frame.
FrameTimeSummary summarizeFrameTimes(std::vector<double> frameTimesMs);

// Packets or frames lost, as a percentage of the total
double lossPercent(uint64_t lost, uint64_t total);

// Averages the per-stage timings of one window into a latency estimate
LatencyBreakdown estimateLatency(const StatsWindow& window);

// Fixed-capacity ring of present timestamps (microseconds). When full, the
// oldest timestamp is overwritten.
class PresentTimeRing
{
public:
    explicit PresentTimeRing(size_t capacity);

    // Timestamps that do not advance are ignored
    void add(uint64_t presentUs);
    void clear();

    size_t size() const { return m_Count; }
    size_t capacity() const { return m_Buffer.size(); }

    // Intervals between consecutive presents, in milliseconds and oldest
    // first, for every pair whose earlier present is at or after sinceUs.
    // An interval longer than maxGapUs is a discontinuity (a minimized window,
    // a decoder reset) rather than a slow frame, and is left out.
    std::vector<double> frameTimesMs(uint64_t sinceUs, uint64_t maxGapUs) const;

private:
    uint64_t at(size_t index) const; // 0 is the oldest

    std::vector<uint64_t> m_Buffer;
    size_t m_Head = 0; // next write position
    size_t m_Count = 0;
};

// Session-long frame time distribution in 0.1 ms buckets. Percentiles are
// exact to the bucket width; times beyond the last bucket report the maximum.
class FrameTimeHistogram
{
public:
    static constexpr double BucketMs = 0.1;
    static constexpr int BucketCount = 2500; // up to 250 ms

    FrameTimeHistogram();

    void add(double frameTimeMs);
    void clear();

    uint64_t count() const { return m_Count; }
    double percentileMs(double p) const;
    FrameTimeSummary summary() const;

private:
    std::vector<uint32_t> m_Buckets;
    uint64_t m_Overflow = 0;
    uint64_t m_Count = 0;
    double m_SumMs = 0;
    double m_MaxMs = 0;
};

// RFC 3550 interarrival jitter applied to whole video frames: the smoothed
// mean deviation between the spacing of frame arrivals and the spacing of
// their RTP timestamps (90 kHz). A perfectly paced stream reads 0.
class JitterEstimator
{
public:
    // Differences larger than this are treated as a stream discontinuity
    static constexpr int64_t MaxTransitDeltaUs = 1000000;

    void addFrame(uint64_t arrivalUs, uint32_t rtpTimestamp);
    void reset();

    double jitterMs() const { return m_JitterUs / 1000.0; }

private:
    bool m_HasPrevious = false;
    uint64_t m_PreviousArrivalUs = 0;
    uint32_t m_PreviousRtpTimestamp = 0;
    double m_JitterUs = 0;
};

} // namespace StatsMath
