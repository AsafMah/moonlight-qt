#include "streamstatsmodel.h"

#include <algorithm>
#include <cmath>

StreamStatsModel::StreamStatsModel() : m_Presents(PresentRingCapacity) {}

void StreamStatsModel::recordFrameArrival(uint64_t receiveTimeUs, uint32_t rtpTimestamp)
{
    std::lock_guard<std::mutex> lock(m_Lock);

    if (m_FirstArrivalUs == 0) {
        m_FirstArrivalUs = receiveTimeUs;
    }
    m_Jitter.addFrame(receiveTimeUs, rtpTimestamp);
}

void StreamStatsModel::recordFramePresented(uint64_t presentTimeUs)
{
    std::lock_guard<std::mutex> lock(m_Lock);

    if (m_LastPresentUs != 0 && presentTimeUs > m_LastPresentUs) {
        const uint64_t intervalUs = presentTimeUs - m_LastPresentUs;
        if (intervalUs <= MaxFrameIntervalUs) {
            m_SessionFrameTimes.add(intervalUs / 1000.0);
        }
    }
    if (presentTimeUs > m_LastPresentUs) {
        m_LastPresentUs = presentTimeUs;
    }

    m_Presents.add(presentTimeUs);
}

void StreamStatsModel::publishWindow(const StatsWindow& recent, const StatsWindow& completed,
                                     uint64_t nowUs)
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        m_Recent = recent;
        m_RecentTimestampUs = nowUs;

        m_SessionFramesTotal += completed.totalFrames;
        m_SessionFramesReceived += completed.receivedFrames;
        m_SessionFramesDecoded += completed.decodedFrames;
        m_SessionFramesRendered += completed.renderedFrames;
        m_SessionFramesNetworkDropped += completed.networkDroppedFrames;
        m_SessionFramesPacerDropped += completed.pacerDroppedFrames;
        m_SessionFramesPacerEligible += StatsMath::pacerDropDenominator(completed);

        // Bumped under the lock so a snapshot's sequence always matches its data
        m_Sequence.fetch_add(1, std::memory_order_acq_rel);
    }
}

StatsSnapshot StreamStatsModel::snapshot(uint64_t nowUs) const
{
    StatsSnapshot snapshot;
    StatsWindow window;
    StatsMath::PresentTimeRing presents(2);

    {
        std::lock_guard<std::mutex> lock(m_Lock);

        snapshot.valid = m_RecentTimestampUs != 0;
        snapshot.sequence = m_Sequence.load(std::memory_order_acquire);
        snapshot.timestampUs = m_RecentTimestampUs;
        if (m_FirstArrivalUs != 0 && nowUs > m_FirstArrivalUs) {
            snapshot.sessionDurationUs = nowUs - m_FirstArrivalUs;
        }

        window = m_Recent;
        presents = m_Presents;
        snapshot.jitterMs = m_Jitter.jitterMs();
        snapshot.sessionFrameTimes = m_SessionFrameTimes.summary();

        snapshot.sessionFramesTotal = m_SessionFramesTotal;
        snapshot.sessionFramesReceived = m_SessionFramesReceived;
        snapshot.sessionFramesDecoded = m_SessionFramesDecoded;
        snapshot.sessionFramesRendered = m_SessionFramesRendered;
        snapshot.sessionFramesNetworkDropped = m_SessionFramesNetworkDropped;
        snapshot.sessionFramesPacerDropped = m_SessionFramesPacerDropped;
        snapshot.sessionFramesPacerEligible = m_SessionFramesPacerEligible;
    }

    // The percentile work happens outside the lock so the render thread is
    // never held up by a consumer
    const uint64_t sinceUs = nowUs > LowFpsWindowUs ? nowUs - LowFpsWindowUs : 0;
    std::vector<double> frameTimes = presents.frameTimesMs(sinceUs, MaxFrameIntervalUs);

    const size_t historyStart =
        frameTimes.size() > FrameTimeHistoryLength ? frameTimes.size() - FrameTimeHistoryLength : 0;
    snapshot.frameTimeHistoryMs.reserve(frameTimes.size() - historyStart);
    for (size_t i = historyStart; i < frameTimes.size(); i++) {
        snapshot.frameTimeHistoryMs.push_back(static_cast<float>(frameTimes[i]));
    }

    snapshot.frameTimes = StatsMath::summarizeFrameTimes(std::move(frameTimes));

    snapshot.width = window.width;
    snapshot.height = window.height;
    snapshot.targetFps = window.targetFps;
    snapshot.codec = window.codec;
    snapshot.decoder = window.decoder;
    snapshot.renderer = window.renderer;
    snapshot.hardwareDecode = window.hardwareDecode;

    snapshot.totalFps = window.totalFps;
    snapshot.receivedFps = window.receivedFps;
    snapshot.decodedFps = window.decodedFps;
    snapshot.renderedFps = window.renderedFps;

    snapshot.hasRtt = window.rttMs != 0;
    snapshot.rttMs = window.rttMs;
    snapshot.rttVarianceMs = window.rttVarianceMs;
    snapshot.networkLossPercent =
        StatsMath::lossPercent(window.networkDroppedFrames, window.totalFrames);
    snapshot.pacerDropPercent =
        StatsMath::lossPercent(window.pacerDroppedFrames, StatsMath::pacerDropDenominator(window));
    snapshot.queueSamples = window.queueMeasured ? window.renderedFrames : 0;
    snapshot.bitrateMbps = window.bitrateMbps;
    snapshot.bitrateTimestampUs = window.bitrateTimestampUs;
    snapshot.videoPackets = window.videoPackets;
    snapshot.fecPackets = window.fecPackets;
    snapshot.fecRecoveredFrames = window.fecRecoveredFrames;
    snapshot.fecUnrecoverableFrames = window.fecUnrecoverableFrames;
    snapshot.presentTiming = StatsMath::estimatePresentTiming(window);

    if (window.receivedFrames > 0) {
        snapshot.reassemblyMs =
            static_cast<double>(window.totalReassemblyTimeUs) / 1000.0 / window.receivedFrames;
    }
    snapshot.hostMinMs = window.minHostProcessingLatency / 10.0;
    snapshot.hostMaxMs = window.maxHostProcessingLatency / 10.0;
    snapshot.latency = StatsMath::estimateLatency(window);

    return snapshot;
}

bool StreamStatsModel::hasFreshReceivedVideoRate(const StatsSnapshot& snapshot, uint64_t nowUs)
{
    return snapshot.valid && std::isfinite(snapshot.bitrateMbps) && snapshot.bitrateMbps >= 0 &&
           snapshot.bitrateTimestampUs != 0 && nowUs >= snapshot.bitrateTimestampUs &&
           nowUs - snapshot.bitrateTimestampUs <= ReceivedVideoRateMaxAgeUs;
}

void StreamStatsModel::reset()
{
    std::lock_guard<std::mutex> lock(m_Lock);

    m_Recent = StatsWindow();
    m_RecentTimestampUs = 0;
    m_FirstArrivalUs = 0;
    m_Jitter.reset();
    m_Presents.clear();
    m_SessionFrameTimes.clear();
    m_LastPresentUs = 0;
    m_SessionFramesTotal = 0;
    m_SessionFramesReceived = 0;
    m_SessionFramesDecoded = 0;
    m_SessionFramesRendered = 0;
    m_SessionFramesNetworkDropped = 0;
    m_SessionFramesPacerDropped = 0;
    m_SessionFramesPacerEligible = 0;
    m_Sequence.store(0, std::memory_order_release);
}
