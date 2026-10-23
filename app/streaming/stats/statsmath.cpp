#include "statsmath.h"

#include <algorithm>
#include <cmath>

namespace StatsMath {

double percentile(std::vector<double>& values, double p)
{
    if (values.empty()) {
        return 0;
    }

    p = std::min(std::max(p, 0.0), 1.0);

    // Nearest rank: the smallest value that at least p of the samples are
    // less than or equal to
    size_t rank = static_cast<size_t>(std::ceil(p * values.size()));
    rank = std::min(std::max(rank, static_cast<size_t>(1)), values.size());

    auto nth = values.begin() + (rank - 1);
    std::nth_element(values.begin(), nth, values.end());
    return *nth;
}

FrameTimeSummary summarizeFrameTimes(std::vector<double> frameTimesMs)
{
    FrameTimeSummary summary;
    if (frameTimesMs.empty()) {
        return summary;
    }

    double sum = 0;
    double max = 0;
    for (double frameTime : frameTimesMs) {
        sum += frameTime;
        max = std::max(max, frameTime);
    }

    summary.samples = frameTimesMs.size();
    summary.averageMs = sum / frameTimesMs.size();
    summary.maxMs = max;
    summary.p99Ms = percentile(frameTimesMs, 0.99);
    summary.p999Ms = percentile(frameTimesMs, 0.999);
    summary.fps = summary.averageMs > 0 ? 1000.0 / summary.averageMs : 0;
    summary.onePercentLowFps = summary.p99Ms > 0 ? 1000.0 / summary.p99Ms : 0;
    summary.pointOnePercentLowFps = summary.p999Ms > 0 ? 1000.0 / summary.p999Ms : 0;
    return summary;
}

double lossPercent(uint64_t lost, uint64_t total)
{
    if (total == 0) {
        return 0;
    }
    return static_cast<double>(lost) * 100.0 / static_cast<double>(total);
}

LatencyBreakdown estimateLatency(const StatsWindow& window)
{
    LatencyBreakdown latency;

    if (window.framesWithHostProcessingLatency > 0) {
        latency.hasHost = true;
        latency.hostMs = static_cast<double>(window.totalHostProcessingLatency) / 10.0 /
                         window.framesWithHostProcessingLatency;
    }

    if (window.rttMs > 0) {
        latency.hasNetwork = true;
        latency.networkMs = window.rttMs / 2.0;
    }

    if (window.decodedFrames > 0) {
        latency.decodeMs =
            static_cast<double>(window.totalDecodeTimeUs) / 1000.0 / window.decodedFrames;
    }

    if (window.renderedFrames > 0) {
        latency.queueMs =
            static_cast<double>(window.totalPacerTimeUs) / 1000.0 / window.renderedFrames;
        latency.renderMs =
            static_cast<double>(window.totalRenderTimeUs) / 1000.0 / window.renderedFrames;
    }

    return latency;
}

PresentTimeRing::PresentTimeRing(size_t capacity) : m_Buffer(std::max(capacity, size_t(2)), 0)
{
}

void PresentTimeRing::add(uint64_t presentUs)
{
    if (m_Count > 0 && presentUs <= at(m_Count - 1)) {
        return;
    }

    m_Buffer[m_Head] = presentUs;
    m_Head = (m_Head + 1) % m_Buffer.size();
    if (m_Count < m_Buffer.size()) {
        m_Count++;
    }
}

void PresentTimeRing::clear()
{
    m_Head = 0;
    m_Count = 0;
}

uint64_t PresentTimeRing::at(size_t index) const
{
    const size_t oldest = (m_Head + m_Buffer.size() - m_Count) % m_Buffer.size();
    return m_Buffer[(oldest + index) % m_Buffer.size()];
}

std::vector<double> PresentTimeRing::frameTimesMs(uint64_t sinceUs, uint64_t maxGapUs) const
{
    std::vector<double> frameTimes;
    if (m_Count < 2) {
        return frameTimes;
    }

    frameTimes.reserve(m_Count - 1);
    for (size_t i = 1; i < m_Count; i++) {
        const uint64_t previous = at(i - 1);
        if (previous < sinceUs) {
            continue;
        }

        const uint64_t interval = at(i) - previous;
        if (interval > maxGapUs) {
            continue;
        }

        frameTimes.push_back(interval / 1000.0);
    }

    return frameTimes;
}

FrameTimeHistogram::FrameTimeHistogram() : m_Buckets(BucketCount, 0)
{
}

void FrameTimeHistogram::add(double frameTimeMs)
{
    if (!(frameTimeMs >= 0)) {
        return;
    }

    // Bucket i holds (i * BucketMs, (i + 1) * BucketMs]
    const double position = std::ceil(frameTimeMs / BucketMs) - 1;
    if (position >= BucketCount) {
        m_Overflow++;
    }
    else {
        m_Buckets[static_cast<size_t>(std::max(position, 0.0))]++;
    }

    m_Count++;
    m_SumMs += frameTimeMs;
    m_MaxMs = std::max(m_MaxMs, frameTimeMs);
}

void FrameTimeHistogram::clear()
{
    std::fill(m_Buckets.begin(), m_Buckets.end(), 0);
    m_Overflow = 0;
    m_Count = 0;
    m_SumMs = 0;
    m_MaxMs = 0;
}

double FrameTimeHistogram::percentileMs(double p) const
{
    if (m_Count == 0) {
        return 0;
    }

    p = std::min(std::max(p, 0.0), 1.0);
    uint64_t rank = static_cast<uint64_t>(std::ceil(p * m_Count));
    rank = std::min(std::max(rank, uint64_t(1)), m_Count);

    uint64_t cumulative = 0;
    for (int i = 0; i < BucketCount; i++) {
        cumulative += m_Buckets[i];
        if (cumulative >= rank) {
            // Report the bucket's upper edge, but never beyond what was seen
            return std::min((i + 1) * BucketMs, m_MaxMs);
        }
    }

    return m_MaxMs;
}

FrameTimeSummary FrameTimeHistogram::summary() const
{
    FrameTimeSummary summary;
    if (m_Count == 0) {
        return summary;
    }

    summary.samples = static_cast<size_t>(m_Count);
    summary.averageMs = m_SumMs / m_Count;
    summary.maxMs = m_MaxMs;
    summary.p99Ms = percentileMs(0.99);
    summary.p999Ms = percentileMs(0.999);
    summary.fps = summary.averageMs > 0 ? 1000.0 / summary.averageMs : 0;
    summary.onePercentLowFps = summary.p99Ms > 0 ? 1000.0 / summary.p99Ms : 0;
    summary.pointOnePercentLowFps = summary.p999Ms > 0 ? 1000.0 / summary.p999Ms : 0;
    return summary;
}

void JitterEstimator::addFrame(uint64_t arrivalUs, uint32_t rtpTimestamp)
{
    if (m_HasPrevious) {
        // The RTP difference is taken as signed 32-bit so it survives wraparound
        const int64_t arrivalDeltaUs =
            static_cast<int64_t>(arrivalUs) - static_cast<int64_t>(m_PreviousArrivalUs);
        const int32_t rtpDelta = static_cast<int32_t>(rtpTimestamp - m_PreviousRtpTimestamp);
        const int64_t rtpDeltaUs = static_cast<int64_t>(rtpDelta) * 1000000 / 90000;
        const int64_t transitDeltaUs = arrivalDeltaUs - rtpDeltaUs;

        if (transitDeltaUs > MaxTransitDeltaUs || transitDeltaUs < -MaxTransitDeltaUs) {
            // A stall or a host-side reset; start over from this frame rather
            // than letting one outlier dominate the estimate for seconds
            m_JitterUs = 0;
        }
        else {
            const double deviation = std::fabs(static_cast<double>(transitDeltaUs));
            m_JitterUs += (deviation - m_JitterUs) / 16.0;
        }
    }

    m_HasPrevious = true;
    m_PreviousArrivalUs = arrivalUs;
    m_PreviousRtpTimestamp = rtpTimestamp;
}

void JitterEstimator::reset()
{
    m_HasPrevious = false;
    m_PreviousArrivalUs = 0;
    m_PreviousRtpTimestamp = 0;
    m_JitterUs = 0;
}

} // namespace StatsMath
