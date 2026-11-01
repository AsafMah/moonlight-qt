#pragma once

// How one complete PyroWave frame is handed to the decoder. Kept apart from
// the Vulkan renderer so the order of the decoder calls can be tested.

namespace PyrowaveFrameFeed {

enum class Result
{
    // Every block arrived; the frame can be decoded.
    Ready,
    // Blocks are missing. Only this frame is lost.
    Incomplete,
    // The decoder refused the payload.
    Rejected,
};

// clear() resets the decoder, push() hands it the whole frame and returns
// false when the decoder refuses it, isReady() asks whether it is complete.
//
// The decoder is cleared before every frame, not only after an incomplete
// one. Its sequence counter is 3 bits and counts encoded frames, and frames
// the client never submits (pacer drops, network losses) leave gaps in it. A
// decoder still holding the last decoded frame takes a complete frame 4 to 8
// (modulo 8) encoded frames later as an older or the same sequence and never
// reports it ready.
template <typename Clear, typename Push, typename IsReady>
Result feed(Clear&& clear, Push&& push, IsReady&& isReady)
{
    clear();
    if (!push()) {
        return Result::Rejected;
    }
    if (!isReady()) {
        return Result::Incomplete;
    }
    return Result::Ready;
}

} // namespace PyrowaveFrameFeed
