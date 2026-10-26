#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "pacerstats.h"
#include "presentstalldetector.h"
#include "../../vrrpolicy.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

#include <memory>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual bool waitForVsync()
    {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
        return false;
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PacerStats* stats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool enableVrr = false);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    void waitForVrrPresentSlot();
    void updateRenderQueueHistoryLocked();

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    SDL_Window* m_Window = nullptr;
#endif
    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PacerStats* m_Stats;
    int m_RendererAttributes;

    // Present-rate cap for VRR mode (disabled otherwise)
    VrrPolicy::FrameLimiter m_VrrLimiter;
    std::unique_ptr<PresentStallDetector> m_PresentStallDetector;
    uint64_t m_DrainSkippedFrames = 0;
    bool m_PostDrainCollectionPending = false;
    bool m_CollectingPostDrainTiming = false;
    uint64_t m_PostDrainStartUs = 0;
    uint64_t m_PostDrainFrames = 0;
    uint64_t m_PostDrainTotalRenderUs = 0;
    uint64_t m_PostDrainTimedFrames = 0;
    uint64_t m_PostDrainTotalPrePresentUs = 0;
    uint64_t m_PostDrainTotalPresentUs = 0;
};
