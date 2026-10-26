#include "sessionsummary.h"

#include "statsmath.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

void SessionSummaryBuilder::addWindow(const StatsSnapshot& snapshot)
{
    if (!snapshot.valid) {
        return;
    }

    m_Windows++;

    if (!std::isnan(snapshot.bitrateMbps) && snapshot.bitrateMbps >= 0) {
        m_BitrateWindows++;
        m_BitrateSumMbps += snapshot.bitrateMbps;
        m_PeakBitrateMbps = std::max(m_PeakBitrateMbps, snapshot.bitrateMbps);
    }

    // A window without decoded frames (a stall, a decoder reset) has no
    // meaningful latency
    if (snapshot.decodedFps > 0) {
        const LatencyBreakdown& latency = snapshot.latency;
        const double totalMs = latency.totalMs();

        m_LatencyWindows++;
        m_LatencySumMs += totalMs;
        m_MaxLatencyMs = std::max(m_MaxLatencyMs, totalMs);
        m_LatencySum.decodeMs += latency.decodeMs;
        m_LatencySum.renderMs += latency.renderMs;
        if (latency.hasHost) {
            m_HostLatencyWindows++;
            m_LatencySum.hostMs += latency.hostMs;
        }
        if (latency.hasNetwork) {
            m_NetworkLatencyWindows++;
            m_LatencySum.networkMs += latency.networkMs;
        }
        if (latency.hasQueue && snapshot.queueSamples > 0) {
            m_QueueLatencySamples += snapshot.queueSamples;
            m_LatencySum.queueMs += latency.queueMs * snapshot.queueSamples;
        }
    }

    if (snapshot.hasRtt) {
        m_RttWindows++;
        m_RttSumMs += snapshot.rttMs;
        m_MaxRttMs = std::max(m_MaxRttMs, snapshot.rttMs);
    }

    m_JitterSumMs += snapshot.jitterMs;
}

void SessionSummaryBuilder::reset()
{
    *this = SessionSummaryBuilder();
}

SessionSummary SessionSummaryBuilder::build(const StatsSnapshot& last) const
{
    SessionSummary summary;

    summary.valid = m_Windows > 0 && last.valid;
    summary.windows = m_Windows;
    summary.durationUs = last.sessionDurationUs;

    summary.width = last.width;
    summary.height = last.height;
    summary.targetFps = last.targetFps;
    summary.codec = last.codec;
    summary.decoder = last.decoder;
    summary.renderer = last.renderer;
    summary.hardwareDecode = last.hardwareDecode;

    summary.frameTimes = last.sessionFrameTimes;

    summary.framesTotal = last.sessionFramesTotal;
    summary.framesReceived = last.sessionFramesReceived;
    summary.framesDecoded = last.sessionFramesDecoded;
    summary.framesRendered = last.sessionFramesRendered;
    summary.framesNetworkDropped = last.sessionFramesNetworkDropped;
    summary.framesPacerDropped = last.sessionFramesPacerDropped;
    summary.framesPacerEligible = last.sessionFramesPacerEligible > 0
                                      ? last.sessionFramesPacerEligible
                                      : last.sessionFramesDecoded;
    summary.networkLossPercent =
        StatsMath::lossPercent(last.sessionFramesNetworkDropped, last.sessionFramesTotal);
    summary.pacerDropPercent =
        StatsMath::lossPercent(last.sessionFramesPacerDropped, summary.framesPacerEligible);

    if (m_BitrateWindows > 0) {
        summary.hasBitrate = true;
        summary.averageBitrateMbps = m_BitrateSumMbps / m_BitrateWindows;
        summary.peakBitrateMbps = m_PeakBitrateMbps;
    }

    if (m_LatencyWindows > 0) {
        summary.hasLatency = true;
        summary.averageLatencyMs = m_LatencySumMs / m_LatencyWindows;
        summary.maxLatencyMs = m_MaxLatencyMs;
        summary.averageLatency.decodeMs = m_LatencySum.decodeMs / m_LatencyWindows;
        summary.averageLatency.renderMs = m_LatencySum.renderMs / m_LatencyWindows;
        if (m_QueueLatencySamples > 0) {
            summary.averageLatency.hasQueue = true;
            summary.averageLatency.queueMs = m_LatencySum.queueMs / m_QueueLatencySamples;
        }
        if (m_HostLatencyWindows > 0) {
            summary.averageLatency.hasHost = true;
            summary.averageLatency.hostMs = m_LatencySum.hostMs / m_HostLatencyWindows;
        }
        if (m_NetworkLatencyWindows > 0) {
            summary.averageLatency.hasNetwork = true;
            summary.averageLatency.networkMs = m_LatencySum.networkMs / m_NetworkLatencyWindows;
        }
    }

    if (m_RttWindows > 0) {
        summary.hasRtt = true;
        summary.averageRttMs = m_RttSumMs / m_RttWindows;
        summary.maxRttMs = m_MaxRttMs;
    }
    if (m_Windows > 0) {
        summary.averageJitterMs = m_JitterSumMs / m_Windows;
    }

    summary.videoPackets = last.videoPackets;
    summary.fecPackets = last.fecPackets;
    summary.fecRecoveredFrames = last.fecRecoveredFrames;
    summary.fecUnrecoverableFrames = last.fecUnrecoverableFrames;

    return summary;
}

QVariantMap SessionSummary::toVariantMap() const
{
    QVariantMap map;

    map.insert(QStringLiteral("valid"), valid);
    map.insert(QStringLiteral("host"), host);
    map.insert(QStringLiteral("app"), app);
    if (startTime.isValid()) {
        map.insert(QStringLiteral("startTime"), startTime.toString(Qt::ISODate));
    }
    if (endTime.isValid()) {
        map.insert(QStringLiteral("endTime"), endTime.toString(Qt::ISODate));
    }
    map.insert(QStringLiteral("durationSeconds"), static_cast<double>(durationUs) / 1000000.0);

    map.insert(QStringLiteral("width"), width);
    map.insert(QStringLiteral("height"), height);
    map.insert(QStringLiteral("targetFps"), targetFps);
    map.insert(QStringLiteral("codec"), codec);
    map.insert(QStringLiteral("decoder"), decoder);
    map.insert(QStringLiteral("renderer"), renderer);
    map.insert(QStringLiteral("hardwareDecode"), hardwareDecode);

    map.insert(QStringLiteral("frameTimeSamples"), static_cast<double>(frameTimes.samples));
    map.insert(QStringLiteral("averageFps"), frameTimes.fps);
    map.insert(QStringLiteral("frameTimeAverageMs"), frameTimes.averageMs);
    map.insert(QStringLiteral("frameTimeMaxMs"), frameTimes.maxMs);
    map.insert(QStringLiteral("frameTimeP99Ms"), frameTimes.p99Ms);
    map.insert(QStringLiteral("frameTimeP999Ms"), frameTimes.p999Ms);
    map.insert(QStringLiteral("onePercentLowFps"), frameTimes.onePercentLowFps);
    map.insert(QStringLiteral("pointOnePercentLowFps"), frameTimes.pointOnePercentLowFps);

    map.insert(QStringLiteral("framesTotal"), static_cast<double>(framesTotal));
    map.insert(QStringLiteral("framesReceived"), static_cast<double>(framesReceived));
    map.insert(QStringLiteral("framesDecoded"), static_cast<double>(framesDecoded));
    map.insert(QStringLiteral("framesRendered"), static_cast<double>(framesRendered));
    map.insert(QStringLiteral("framesNetworkDropped"), static_cast<double>(framesNetworkDropped));
    map.insert(QStringLiteral("framesPacerDropped"), static_cast<double>(framesPacerDropped));
    map.insert(QStringLiteral("framesPacerEligible"), static_cast<double>(framesPacerEligible));
    map.insert(QStringLiteral("networkLossPercent"), networkLossPercent);
    map.insert(QStringLiteral("pacerDropPercent"), pacerDropPercent);

    if (hasBitrate) {
        map.insert(QStringLiteral("averageBitrateMbps"), averageBitrateMbps);
        map.insert(QStringLiteral("peakBitrateMbps"), peakBitrateMbps);
    }

    if (hasLatency) {
        map.insert(QStringLiteral("averageLatencyMs"), averageLatencyMs);
        map.insert(QStringLiteral("maxLatencyMs"), maxLatencyMs);
        if (averageLatency.hasHost) {
            map.insert(QStringLiteral("latencyHostMs"), averageLatency.hostMs);
        }
        if (averageLatency.hasNetwork) {
            map.insert(QStringLiteral("latencyNetworkMs"), averageLatency.networkMs);
        }
        map.insert(QStringLiteral("latencyDecodeMs"), averageLatency.decodeMs);
        if (averageLatency.hasQueue) {
            map.insert(QStringLiteral("latencyQueueMs"), averageLatency.queueMs);
        }
        map.insert(QStringLiteral("latencyRenderMs"), averageLatency.renderMs);
    }

    if (hasRtt) {
        map.insert(QStringLiteral("averageRttMs"), averageRttMs);
        map.insert(QStringLiteral("maxRttMs"), maxRttMs);
    }
    map.insert(QStringLiteral("averageJitterMs"), averageJitterMs);

    map.insert(QStringLiteral("videoPackets"), videoPackets);
    map.insert(QStringLiteral("fecPackets"), fecPackets);
    if (fecRecoveredFrames >= 0) {
        map.insert(QStringLiteral("fecRecoveredFrames"), static_cast<double>(fecRecoveredFrames));
    }
    if (fecUnrecoverableFrames >= 0) {
        map.insert(QStringLiteral("fecUnrecoverableFrames"),
                   static_cast<double>(fecUnrecoverableFrames));
    }

    map.insert(QStringLiteral("windows"), static_cast<double>(windows));
    if (!csvPath.isEmpty()) {
        map.insert(QStringLiteral("csvPath"), csvPath);
    }

    return map;
}

QString SessionSummary::toText() const
{
    if (!valid) {
        return QStringLiteral("No stream statistics were recorded");
    }

    const uint64_t totalSeconds = durationUs / 1000000;
    const QString duration = QStringLiteral("%1:%2:%3")
                                 .arg(totalSeconds / 3600)
                                 .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
                                 .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));

    QStringList lines;
    lines << QStringLiteral("Session: %1 on %2, %3").arg(app, host, duration);
    lines << QStringLiteral("Stream: %1x%2 at %3 FPS, %4 (%5)")
                 .arg(width)
                 .arg(height)
                 .arg(targetFps)
                 .arg(codec, decoder);
    lines << QStringLiteral("Frame times: %1 ms average, %2 ms 99th, %3 ms 99.9th, %4 ms max")
                 .arg(frameTimes.averageMs, 0, 'f', 2)
                 .arg(frameTimes.p99Ms, 0, 'f', 2)
                 .arg(frameTimes.p999Ms, 0, 'f', 2)
                 .arg(frameTimes.maxMs, 0, 'f', 2);
    lines << QStringLiteral("FPS: %1 average, %2 1% low, %3 0.1% low")
                 .arg(frameTimes.fps, 0, 'f', 1)
                 .arg(frameTimes.onePercentLowFps, 0, 'f', 1)
                 .arg(frameTimes.pointOnePercentLowFps, 0, 'f', 1);
    lines << QStringLiteral("Frames: %1 total, %2 lost by the network (%3%), %4 dropped by the "
                            "pacer (%5%; %4 of %6 eligible frames)")
                 .arg(framesTotal)
                 .arg(framesNetworkDropped)
                 .arg(networkLossPercent, 0, 'f', 2)
                 .arg(framesPacerDropped)
                 .arg(pacerDropPercent, 0, 'f', 2)
                 .arg(framesPacerEligible);
    if (hasBitrate) {
        lines << QStringLiteral("Bitrate: %1 Mbps average, %2 Mbps peak")
                     .arg(averageBitrateMbps, 0, 'f', 2)
                     .arg(peakBitrateMbps, 0, 'f', 2);
    }
    if (hasLatency) {
        lines << QStringLiteral("Estimated latency (not glass-to-glass): %1 ms average, "
                                "%2 ms worst window")
                     .arg(averageLatencyMs, 0, 'f', 1)
                     .arg(maxLatencyMs, 0, 'f', 1);
        lines << (averageLatency.hasQueue ? QStringLiteral("Pacer queue: %1 ms average")
                                                .arg(averageLatency.queueMs, 0, 'f', 1)
                                          : QStringLiteral("Pacer queue: Not measured"));
    }
    if (hasRtt) {
        lines << QStringLiteral("Network: %1 ms average RTT, %2 ms max, %3 ms jitter")
                     .arg(averageRttMs, 0, 'f', 1)
                     .arg(maxRttMs)
                     .arg(averageJitterMs, 0, 'f', 2);
    }
    QString fec = QStringLiteral("FEC: %1 of %2 packets").arg(fecPackets).arg(videoPackets);
    if (fecRecoveredFrames >= 0 && fecUnrecoverableFrames >= 0) {
        fec += QStringLiteral(", %1 frames recovered, %2 unrecoverable")
                   .arg(fecRecoveredFrames)
                   .arg(fecUnrecoverableFrames);
    }
    lines << fec;
    if (!csvPath.isEmpty()) {
        lines << QStringLiteral("CSV: %1").arg(csvPath);
    }

    return lines.join(QLatin1Char('\n'));
}
