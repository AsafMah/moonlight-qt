#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>

// Splits the frame-number gap a decoder sees between two dequeued frames into
// frames the network lost and frames moonlight-common-c received completely but
// discarded itself: decode unit queue overflow, the DR_NEED_IDR queue flush and
// the IDR wait that follows. Both decoders feed it LiGetVideoLocalDiscardStats(),
// read after the decode unit was dequeued; it is a template only so tests can
// use it without Limelight.h.
struct FrameGapSplit
{
    uint32_t networkDropped = 0;
    uint32_t localDropped = 0;
};

class FrameGapClassifier
{
public:
    // First frame of the stream: earlier discards left no gap at the decoder.
    template <typename Discards> void start(const Discards& discards)
    {
        m_ConsumedRanges = discards.rangesRecorded;
    }

    template <typename Discards>
    FrameGapSplit classify(uint32_t frameNumber, uint32_t gap, const Discards& discards)
    {
        const uint64_t capacity = std::size(discards.ranges);
        if (discards.rangesRecorded < m_ConsumedRanges) {
            // The counters restarted with a new connection
            m_ConsumedRanges = 0;
        }

        // Ranges pushed out of the ring before they were seen can't be placed.
        // Treat the gap as local then: a false network loss would make the
        // host lower the bitrate.
        uint64_t next = m_ConsumedRanges;
        bool unknownRanges = false;
        if (discards.rangesRecorded - next > capacity) {
            unknownRanges = true;
            next = discards.rangesRecorded - capacity;
        }

        // Discards are recorded in frame order. Each range lies entirely before
        // or after this frame, and one after it was discarded after this frame
        // was dequeued, so it belongs to a later gap (wrap-safe comparison).
        uint64_t localFrames = 0;
        for (; next < discards.rangesRecorded; next++) {
            const auto& range = discards.ranges[next % capacity];
            if (static_cast<int32_t>(frameNumber - range.lastFrameNumber) <= 0) {
                break;
            }
            localFrames += range.frames;
        }
        m_ConsumedRanges = next;

        FrameGapSplit split;
        split.localDropped =
            unknownRanges ? gap : static_cast<uint32_t>(std::min<uint64_t>(gap, localFrames));
        split.networkDropped = gap - split.localDropped;
        return split;
    }

private:
    uint64_t m_ConsumedRanges = 0;
};

// Adds a classified gap to a VIDEO_STATS or StatsWindow. Gap frames stay in
// totalFrames whichever side dropped them.
template <typename Stats> void addFrameGap(Stats& stats, const FrameGapSplit& split)
{
    stats.networkDroppedFrames += split.networkDropped;
    stats.localDroppedFrames += split.localDropped;
    stats.totalFrames += split.networkDropped + split.localDropped;
}
