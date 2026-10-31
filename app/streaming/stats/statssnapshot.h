#pragma once

#include <QString>

#include <cmath>
#include <cstdint>
#include <vector>

// Plain data types shared by StreamStatsModel, its producers (the video
// decoder and Pacer) and its consumers (the stats overlay, logging).
//
// Nothing here depends on SDL or moonlight-common-c, so the model and its math
// can be unit tested with QtCore alone (see tests/stats_model).

// One measurement window of decoder counters, mirrored from VIDEO_STATS. The
// decoder publishes one roughly every second. Durations are totals over the
// window; divide by the matching frame count for an average.
struct StatsWindow
{
    // Stream description at the time of the window
    int width = 0;
    int height = 0;
    int targetFps = 0; // negotiated stream frame rate
    QString codec;     // e.g. "HEVC 10-bit HDR"
    QString decoder;   // e.g. "hevc (d3d11va)"
    QString renderer;  // IFFmpegRenderer::getRendererName()
    bool hardwareDecode = false;

    // Frame counters
    uint32_t receivedFrames = 0;
    uint32_t decodedFrames = 0;
    uint32_t renderedFrames = 0;
    uint32_t totalFrames = 0;
    uint32_t networkDroppedFrames = 0;
    uint32_t pacerDroppedFrames = 0;
    // Frames eligible for a Pacer drop. PyroWave can drop before decode, so
    // this may exceed decodedFrames; zero falls back to decodedFrames.
    uint32_t pacerDropEligibleFrames = 0;

    // Host processing latency, in units of 0.1 ms as reported over RTP
    uint32_t framesWithHostProcessingLatency = 0;
    uint64_t totalHostProcessingLatency = 0;
    uint16_t minHostProcessingLatency = 0;
    uint16_t maxHostProcessingLatency = 0;

    uint64_t totalReassemblyTimeUs = 0;
    uint64_t totalDecodeTimeUs = 0;
    uint64_t totalPacerTimeUs = 0;
    uint64_t totalRenderTimeUs = 0;
    // Timed successful presents; the separate denominator excludes failed or unavailable samples.
    uint32_t timedRenderFrames = 0;
    uint64_t totalPrePresentTimeUs = 0;
    uint64_t totalPresentTimeUs = 0;
    bool queueMeasured = true;

    // ENet RTT estimate; 0 when not known yet
    uint32_t rttMs = 0;
    uint32_t rttVarianceMs = 0;

    double totalFps = 0;
    double receivedFps = 0;
    double decodedFps = 0;
    double renderedFps = 0;

    // Received video-socket rate in Mbps, including FEC and video packet/encryption overhead.
    // This is not the host encoder bitrate budget; NaN if no valid sample.
    double bitrateMbps = std::nan("");
    uint64_t bitrateTimestampUs = 0; // Time of the last valid bandwidth sample

    // Cumulative RTP packet counters for the video stream (since stream start)
    uint32_t videoPackets = 0;
    uint32_t fecPackets = 0;
    // Cumulative frames that were only complete after FEC repaired them, and
    // partly received frames FEC could not repair. -1 means the transport
    // does not report them (stock moonlight-common-c counts FEC results for
    // audio only).
    int64_t fecRecoveredFrames = -1;
    int64_t fecUnrecoverableFrames = -1;
};

// Estimated client-side pipeline latency, split into the stages we can measure
// or estimate. This is not a glass-to-glass measurement.
struct LatencyBreakdown
{
    double hostMs = 0;    // capture + encode on the host (reported over RTP)
    double networkMs = 0; // half the round trip time
    double decodeMs = 0;  // decode queue + decode
    double queueMs = 0;   // Pacer queue
    double renderMs = 0;  // render, including V-sync wait where the renderer blocks

    bool hasHost = false;    // the host reported processing latency
    bool hasNetwork = false; // an RTT estimate exists
    bool hasQueue = false;   // the decoder measured time spent in the Pacer queue

    double totalMs() const
    {
        return hostMs + networkMs + decodeMs + (hasQueue ? queueMs : 0) + renderMs;
    }
};

// Frame pacing as seen at present time
struct FrameTimeSummary
{
    size_t samples = 0; // number of frame times the values come from
    double averageMs = 0;
    double maxMs = 0;
    double p99Ms = 0;                 // 99th percentile frame time
    double p999Ms = 0;                // 99.9th percentile frame time
    double fps = 0;                   // 1000 / averageMs
    double onePercentLowFps = 0;      // 1000 / p99Ms
    double pointOnePercentLowFps = 0; // 1000 / p999Ms
};

struct PresentTiming
{
    bool measured = false;
    uint32_t samples = 0;
    double prePresentMs = 0;
    double presentMs = 0;
};

// Everything the overlay and the logs show, in one copyable value
struct StatsSnapshot
{
    // False until the decoder has published its first window
    bool valid = false;
    // Increments with every published window, so a poller can tell new data
    uint64_t sequence = 0;
    // LiGetMicroseconds() time of the last published window
    uint64_t timestampUs = 0;
    // Time since the first frame arrived
    uint64_t sessionDurationUs = 0;

    // Stream description
    int width = 0;
    int height = 0;
    int targetFps = 0;
    QString codec;
    QString decoder;
    QString renderer;
    bool hardwareDecode = false;

    // Frame rates over the last two windows (about two seconds)
    double totalFps = 0;
    double receivedFps = 0;
    double decodedFps = 0;
    double renderedFps = 0;

    // Present-time frame pacing over the last StreamStatsModel::LowFpsWindowUs
    FrameTimeSummary frameTimes;
    // The same, over the whole session (from a histogram, 0.1 ms resolution)
    FrameTimeSummary sessionFrameTimes;
    // Most recent frame times in ms, oldest first, for a frame time graph
    std::vector<float> frameTimeHistoryMs;

    // Network
    bool hasRtt = false;
    uint32_t rttMs = 0;
    uint32_t rttVarianceMs = 0;
    double jitterMs = 0;           // RFC 3550 interarrival jitter of video frames
    double networkLossPercent = 0; // frames lost by the network
    double pacerDropPercent = 0;   // decoded frames dropped by the Pacer
    uint32_t queueSamples = 0;     // rendered frames with a measured Pacer queue time
    double bitrateMbps = std::nan("");
    uint64_t bitrateTimestampUs = 0;
    uint32_t videoPackets = 0;
    uint32_t fecPackets = 0;
    int64_t fecRecoveredFrames = -1;
    int64_t fecUnrecoverableFrames = -1;

    // Per-stage averages over the last two windows
    PresentTiming presentTiming;
    double reassemblyMs = 0; // first to last packet of a frame
    double hostMinMs = 0;
    double hostMaxMs = 0;
    LatencyBreakdown latency;

    // Session totals
    uint64_t sessionFramesTotal = 0;
    uint64_t sessionFramesReceived = 0;
    uint64_t sessionFramesDecoded = 0;
    uint64_t sessionFramesRendered = 0;
    uint64_t sessionFramesNetworkDropped = 0;
    uint64_t sessionFramesPacerDropped = 0;
    uint64_t sessionFramesPacerEligible = 0;
};
