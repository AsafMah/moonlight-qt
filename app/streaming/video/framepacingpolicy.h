#pragma once

// Frame pacing (V-Sync on, without VRR) policy. Pure so it can be unit tested.
namespace FramePacing {

// Decides when a decoded frame leaves Pacer's pacing queue for the render
// thread. Called for every decoded frame and on every V-blank.
//
// A frame goes to the render thread as soon as it is decoded. The render
// thread then waits until the renderer can take a frame (on D3D11 the
// swapchain's frame latency waitable, signalled once the previous present
// was latched at a V-blank) and presents only the newest queued frame,
// dropping older ones. So at most one frame is presented per V-blank, and a
// frame is only dropped when a newer one is already waiting.
//
// Frames used to be held until the next V-blank unless they arrived within
// (1000/Hz - 3) whole ms of a V-blank at which nothing was released. Once a
// frame was released at a V-blank, every on-time frame after it was held to
// the next V-blank too: up to a whole refresh (8.3 ms at 120 Hz) of added
// queue for as long as each refresh got a frame.
class VblankGate
{
public:
    // A frame was decoded. True: hand it to the render thread now; false:
    // keep it in the pacing queue until the next V-blank.
    bool releaseOnArrival() const { return true; }

    // A V-blank. True: hand the frame still in the pacing queue (if any) to
    // the render thread now.
    bool onVblank(bool haveQueuedFrame) const { return haveQueuedFrame; }
};

// Whether the render thread presents only the newest queued frame (dropping
// older ones) rather than the oldest: when the renderer reported it can
// present without blocking, or with frame pacing. Not for VRR, which has its
// own rule, or renderers that need a frame buffered (no waitToRender()).
constexpr bool presentNewestOnly(bool vrr, bool readyToPresentAfterWait, bool framePacing,
                                 bool noBuffering)
{
    return !vrr && !noBuffering && (readyToPresentAfterWait || framePacing);
}

} // namespace FramePacing
