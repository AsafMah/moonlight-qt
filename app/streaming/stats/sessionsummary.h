#pragma once

#include "statssnapshot.h"

#include <QDateTime>
#include <QString>
#include <QVariantMap>

#include <cstdint>

// What a stream looked like from start to end, built from the stats windows
// StreamStatsModel published during it (see SessionSummaryBuilder). Shown
// after the stream and written to the log.
struct SessionSummary
{
    // False when no stats window was published (the stream never got going)
    bool valid = false;

    QString host;
    QString app;
    QDateTime startTime; // wall clock, when recording started
    QDateTime endTime;   // wall clock, when recording stopped
    uint64_t durationUs = 0; // since the first frame arrived

    // Stream description from the last window
    int width = 0;
    int height = 0;
    int targetFps = 0;
    QString codec;
    QString decoder;
    QString renderer;
    bool hardwareDecode = false;

    // Whole-session present-time frame pacing, including 1% and 0.1% lows
    FrameTimeSummary frameTimes;

    // Session frame totals
    uint64_t framesTotal = 0;
    uint64_t framesReceived = 0;
    uint64_t framesDecoded = 0;
    uint64_t framesRendered = 0;
    uint64_t framesNetworkDropped = 0;
    uint64_t framesPacerDropped = 0;
    double networkLossPercent = 0; // of framesTotal
    double pacerDropPercent = 0;   // of framesDecoded

    // Averaged over the windows that had a value
    bool hasBitrate = false;
    double averageBitrateMbps = 0;
    double peakBitrateMbps = 0;

    bool hasLatency = false;
    LatencyBreakdown averageLatency; // per-stage averages
    double averageLatencyMs = 0;     // average of each window's total
    double maxLatencyMs = 0;         // the worst window's total

    bool hasRtt = false;
    double averageRttMs = 0;
    uint32_t maxRttMs = 0;
    double averageJitterMs = 0;

    // Cumulative counters at the end of the stream. The FEC frame counts are
    // -1 when the transport does not report them.
    uint32_t videoPackets = 0;
    uint32_t fecPackets = 0;
    int64_t fecRecoveredFrames = -1;
    int64_t fecUnrecoverableFrames = -1;

    // Number of stats windows the summary is built from
    uint64_t windows = 0;

    // The CSV this session was recorded to, if any
    QString csvPath;

    // All fields under their member names, for QML. Unknown values (no
    // bitrate, no RTT, unreported FEC) are left out rather than zeroed.
    QVariantMap toVariantMap() const;

    // A few lines of plain English text, for the log
    QString toText() const;
};

// Accumulates the per-window values a SessionSummary averages. Feed it one
// snapshot per published window (a new StatsSnapshot::sequence), then call
// build() with the last snapshot of the stream.
class SessionSummaryBuilder
{
public:
    void addWindow(const StatsSnapshot& snapshot);
    void reset();

    uint64_t windows() const { return m_Windows; }

    // Session totals and the stream description come from last, which should
    // be taken when the stream ended
    SessionSummary build(const StatsSnapshot& last) const;

private:
    uint64_t m_Windows = 0;

    uint64_t m_BitrateWindows = 0;
    double m_BitrateSumMbps = 0;
    double m_PeakBitrateMbps = 0;

    uint64_t m_LatencyWindows = 0;
    double m_LatencySumMs = 0;
    double m_MaxLatencyMs = 0;
    LatencyBreakdown m_LatencySum;
    uint64_t m_HostLatencyWindows = 0;
    uint64_t m_NetworkLatencyWindows = 0;

    uint64_t m_RttWindows = 0;
    double m_RttSumMs = 0;
    uint32_t m_MaxRttMs = 0;

    double m_JitterSumMs = 0;
};
