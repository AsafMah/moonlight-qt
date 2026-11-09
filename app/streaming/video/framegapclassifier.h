#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>

// Splits the frame-number gap a decoder sees between two dequeued frames into
// frames the host never sent, frames moonlight-common-c received completely but
// discarded itself (decode unit queue overflow, the DR_NEED_IDR queue flush and
// the IDR wait that follows) and frames the network lost. Both decoders feed it
// LiGetVideoLocalDiscardStats() and LiGetVideoHostSkipStats(), read after the
// decode unit was dequeued; it is a template only so tests can use it without
// Limelight.h.
struct FrameGapSplit
{
    uint32_t networkDropped = 0;
    uint32_t localDropped = 0;
    uint32_t hostSkipped = 0;
};

class FrameGapClassifier
{
public:
    // First frame of the stream: earlier discards left no gap at the decoder.
    template <typename Discards> void start(const Discards& discards)
    {
        m_ConsumedRanges = discards.rangesRecorded;
    }

    template <typename Discards, typename HostSkips>
    void start(const Discards& discards, const HostSkips& hostSkips)
    {
        start(discards);
        m_ConsumedHostRanges = hostSkips.rangesRecorded;
    }

    template <typename Discards>
    FrameGapSplit classify(uint32_t frameNumber, uint32_t gap, const Discards& discards)
    {
        uint64_t localFrames = 0;
        const bool unknownRanges =
            !consumeRanges(m_ConsumedRanges, frameNumber, discards,
                           [&](const auto& range) { localFrames += range.frames; });

        // Ranges pushed out of the ring before they were seen can't be placed.
        // Treat the gap as local then: a false network loss would make the
        // host lower the bitrate.
        FrameGapSplit split;
        split.localDropped =
            unknownRanges ? gap : static_cast<uint32_t>(std::min<uint64_t>(gap, localFrames));
        split.networkDropped = gap - split.localDropped;
        return split;
    }

    template <typename Discards, typename HostSkips>
    FrameGapSplit classify(uint32_t frameNumber, uint32_t gap, const Discards& discards,
                           const HostSkips& hostSkips)
    {
        // Only the part of a skipped range inside this gap counts: a range can
        // be recorded late (PyroWave blocks arrive out of order) after the
        // decoder already moved past it. Unlike lost discard ranges,
        // overwritten skip ranges stay network loss: the client received the
        // frame after each one, so an overflow means sent frames arrived
        // between many skips and may have been lost. Hiding them would mask
        // congestion.
        uint64_t hostFrames = 0;
        consumeRanges(m_ConsumedHostRanges, frameNumber, hostSkips, [&](const auto& range) {
            // Distances back from this frame: the gap covers 1..gap
            const int64_t newest = static_cast<int32_t>(frameNumber - range.lastFrameNumber);
            const int64_t oldest = static_cast<int32_t>(frameNumber - range.firstFrameNumber);
            if (newest <= gap) {
                hostFrames += (std::min<int64_t>)(oldest, gap) - newest + 1;
            }
        });

        FrameGapSplit split = classify(frameNumber, gap, discards);
        split.hostSkipped = static_cast<uint32_t>((std::min<uint64_t>)(gap, hostFrames));

        // Skipped frames are never local discards: those were received in full
        split.hostSkipped = (std::min)(split.hostSkipped, gap - split.localDropped);
        split.networkDropped = gap - split.localDropped - split.hostSkipped;
        return split;
    }

private:
    // Visits the unconsumed ranges that end before frameNumber and returns
    // false if some were pushed out of the ring before they were seen.
    template <typename Ranges, typename Visit>
    static bool consumeRanges(uint64_t& consumed, uint32_t frameNumber, const Ranges& ranges,
                              Visit visit)
    {
        const uint64_t capacity = std::size(ranges.ranges);
        if (ranges.rangesRecorded < consumed) {
            // The counters restarted with a new connection
            consumed = 0;
        }

        uint64_t next = consumed;
        bool complete = true;
        if (ranges.rangesRecorded - next > capacity) {
            complete = false;
            next = ranges.rangesRecorded - capacity;
        }

        // Ranges are recorded in frame order. Each range lies entirely before
        // or after this frame, and one after it was recorded after this frame
        // was dequeued, so it belongs to a later gap (wrap-safe comparison).
        for (; next < ranges.rangesRecorded; next++) {
            const auto& range = ranges.ranges[next % capacity];
            if (static_cast<int32_t>(frameNumber - range.lastFrameNumber) <= 0) {
                break;
            }
            visit(range);
        }
        consumed = next;
        return complete;
    }

    uint64_t m_ConsumedRanges = 0;
    uint64_t m_ConsumedHostRanges = 0;
};

// Adds a classified gap to a VIDEO_STATS or StatsWindow. Lost and discarded
// gap frames stay in totalFrames; frames the host never sent are counted apart
// and left out of it, so they never dilute or add to the loss percentage.
template <typename Stats> void addFrameGap(Stats& stats, const FrameGapSplit& split)
{
    stats.networkDroppedFrames += split.networkDropped;
    stats.localDroppedFrames += split.localDropped;
    stats.hostSkippedFrames += split.hostSkipped;
    stats.totalFrames += split.networkDropped + split.localDropped;
}
