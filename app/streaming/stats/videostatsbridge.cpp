#include "videostatsbridge.h"
#include "statsmath.h"

#include <cstdio>

namespace VideoStatsBridge {

StatsWindow toStatsWindow(const VIDEO_STATS& stats)
{
    StatsWindow window;

    window.receivedFrames = stats.receivedFrames;
    window.decodedFrames = stats.decodedFrames;
    window.renderedFrames = stats.renderedFrames;
    window.totalFrames = stats.totalFrames;
    window.networkDroppedFrames = stats.networkDroppedFrames;
    window.pacerDroppedFrames = stats.pacerDroppedFrames;

    window.framesWithHostProcessingLatency = stats.framesWithHostProcessingLatency;
    window.totalHostProcessingLatency = stats.totalHostProcessingLatency;
    window.minHostProcessingLatency = stats.minHostProcessingLatency;
    window.maxHostProcessingLatency = stats.maxHostProcessingLatency;

    window.totalReassemblyTimeUs = stats.totalReassemblyTimeUs;
    window.totalDecodeTimeUs = stats.totalDecodeTimeUs;
    window.totalPacerTimeUs = stats.totalPacerTimeUs;
    window.totalRenderTimeUs = stats.totalRenderTimeUs;

    window.rttMs = stats.lastRtt;
    window.rttVarianceMs = stats.lastRttVariance;

    window.totalFps = stats.totalFps;
    window.receivedFps = stats.receivedFps;
    window.decodedFps = stats.decodedFps;
    window.renderedFps = stats.renderedFps;

    return window;
}

QString videoFormatName(int videoFormat, bool hdrActive)
{
    const char* dynamicRange = hdrActive ? "HDR" : "SDR";

    switch (videoFormat) {
    case VIDEO_FORMAT_H264:
        return QStringLiteral("H.264");
    case VIDEO_FORMAT_H264_HIGH8_444:
        return QStringLiteral("H.264 4:4:4");
    case VIDEO_FORMAT_H265:
        return QStringLiteral("HEVC");
    case VIDEO_FORMAT_H265_REXT8_444:
        return QStringLiteral("HEVC 4:4:4");
    case VIDEO_FORMAT_H265_MAIN10:
        return QStringLiteral("HEVC 10-bit %1").arg(QLatin1String(dynamicRange));
    case VIDEO_FORMAT_H265_REXT10_444:
        return QStringLiteral("HEVC 10-bit %1 4:4:4").arg(QLatin1String(dynamicRange));
    case VIDEO_FORMAT_AV1_MAIN8:
        return QStringLiteral("AV1");
    case VIDEO_FORMAT_AV1_HIGH8_444:
        return QStringLiteral("AV1 4:4:4");
    case VIDEO_FORMAT_AV1_MAIN10:
        return QStringLiteral("AV1 10-bit %1").arg(QLatin1String(dynamicRange));
    case VIDEO_FORMAT_AV1_HIGH10_444:
        return QStringLiteral("AV1 10-bit %1 4:4:4").arg(QLatin1String(dynamicRange));
    default:
        return QStringLiteral("UNKNOWN");
    }
}

void readRtpCounters(StatsWindow& window)
{
    const RTP_VIDEO_STATS* rtpStats = LiGetRTPVideoStats();
    if (rtpStats == nullptr) {
        return;
    }

    window.videoPackets = rtpStats->packetCountVideo;
    window.fecPackets = rtpStats->packetCountFec;

    // RTP_VIDEO_STATS has fields for FEC recovered and failed packets, but
    // moonlight-common-c only counts them for audio. Leave the FEC results
    // unreported (-1) rather than showing a misleading zero.
    window.fecRecoveredFrames = -1;
    window.fecUnrecoverableFrames = -1;

#ifdef LI_HAS_RTP_VIDEO_FEC_STATS
    // Moonlight V+: moonlight-common-c builds that count video FEC results
    // (LI_HAS_RTP_VIDEO_FEC_STATS) report them per frame
    RTP_VIDEO_FEC_STATS fecStats = {};
    LiGetRTPVideoFecStats(&fecStats);
    window.fecRecoveredFrames = static_cast<int64_t>(fecStats.framesRecovered);
    window.fecUnrecoverableFrames = static_cast<int64_t>(fecStats.framesUnrecoverable);
#endif
}

int formatMetrics(const StatsSnapshot& snapshot, const VIDEO_STATS& stats, bool sessionWide,
                  char* output, int length)
{
    const FrameTimeSummary& frameTimes =
        sessionWide ? snapshot.sessionFrameTimes : snapshot.frameTimes;
    // Estimated from the same window as the rest of the text
    const LatencyBreakdown latency = StatsMath::estimateLatency(toStatsWindow(stats));

    char lowsString[64];
    if (frameTimes.samples > 0) {
        snprintf(lowsString, sizeof(lowsString), "%.1f/%.1f FPS", frameTimes.onePercentLowFps,
                 frameTimes.pointOnePercentLowFps);
    } else {
        snprintf(lowsString, sizeof(lowsString), "N/A");
    }

    // Stages we have no measurement for are left out of both the sum and
    // the breakdown, rather than counted as zero
    char hostString[24] = "";
    char networkString[24] = "";
    if (latency.hasHost) {
        snprintf(hostString, sizeof(hostString), "host %.1f + ", latency.hostMs);
    }
    if (latency.hasNetwork) {
        snprintf(networkString, sizeof(networkString), "network %.1f + ", latency.networkMs);
    }

    return snprintf(output, length,
                    "Frame rate 1%%/0.1%% lows: %s\n"
                    "Network jitter: %.2f ms\n"
                    "Estimated latency: %.1f ms (%s%sdecode %.1f + queue %.1f + render %.1f)\n",
                    lowsString, snapshot.jitterMs, latency.totalMs(), hostString, networkString,
                    latency.decodeMs, latency.queueMs, latency.renderMs);
}

} // namespace VideoStatsBridge
