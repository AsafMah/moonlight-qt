#include "pacer.h"
#include "streaming/streamutils.h"
#include "streaming/session.h"

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
#include "streaming/waylandqtsdlbridge.h"
#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer, PacerStats* stats)
    : m_RenderThread(nullptr), m_VsyncThread(nullptr), m_DeferredFreeFrame(nullptr),
      m_Stopping(false), m_VsyncSource(nullptr), m_VsyncRenderer(renderer), m_MaxVideoFps(0),
      m_DisplayFps(0), m_Stats(stats)
{

}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            QMutexLocker locker(&me->m_FrameQueueLock);
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
        }
        else {
            // Let the VSync source wait in the context of our thread
            if (!me->m_VsyncSource->waitForVsync()) {
                // Fall back to a fixed delay if the VSync source fails
                //
                // NB: We don't actually expect signalVsync() to be called, since
                // this is a synchronous VSync source, but we wait on m_VsyncSignalled
                // anyway so that the destructor can immediately wake us on shutdown.
                QMutexLocker locker(&me->m_FrameQueueLock);
                me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 1000 / me->m_DisplayFps);
            }
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        AVFrame* frame;
        {
            // Acquire the frame queue lock to protect the queue and
            // the not empty condition
            QMutexLocker locker(&me->m_FrameQueueLock);

            // Wait for a frame to be ready to render
            while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
                me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
            }

            if (me->m_Stopping) {
                // Exit this thread
                break;
            }

            me->waitForVrrPresentSlot();
            if (me->m_Stopping) {
                break;
            }

            frame = me->m_RenderQueue.dequeue();
        }

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_Stats->recordDrop();
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping || m_PacingQueue.isEmpty()) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool enableVrr)
{
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    m_Window = window;
#endif
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (enableVrr) {
        // VRR presents each frame on arrival. The only wait is a cap just below
        // the refresh rate so presents stay inside the display's VRR range.
        SDL_assert(m_VsyncSource == nullptr);
        if (m_VsyncRenderer->isRenderThreadSupported()) {
            m_VrrLimiter = VrrPolicy::FrameLimiter(VrrPolicy::frameCapFps(m_DisplayFps));
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "VRR mode: presenting on arrival, capped at %d FPS for a %d Hz display",
                        VrrPolicy::frameCapFps(m_DisplayFps), m_DisplayFps);
        } else {
            // Never block the main thread to space out presents
            SDL_LogInfo(
                SDL_LOG_CATEGORY_APPLICATION,
                "VRR mode: presenting on arrival without a frame rate cap (main thread renderer)");
        }
    }

    if (qgetenv("MOONLIGHT_PRESENT_DRAIN") == "1" &&
        m_VsyncRenderer->getRendererType() == IFFmpegRenderer::RendererType::D3D11VA &&
        m_VsyncRenderer->isRenderThreadSupported()) {
        const bool vrrLimiterActive = enableVrr && m_VrrLimiter.isEnabled();
        const unsigned int vrrCapFps =
            vrrLimiterActive ? static_cast<unsigned int>(VrrPolicy::frameCapFps(m_DisplayFps)) : 0;
        m_PresentStallDetector = std::make_unique<PresentStallDetector>(
            static_cast<unsigned int>(SDL_max(maxVideoFps, 0)),
            static_cast<unsigned int>(SDL_max(m_DisplayFps, 0)), m_VsyncSource != nullptr,
            vrrLimiterActive, vrrCapFps);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "MOONLIGHT_PRESENT_DRAIN=1: armed one-shot D3D11 experiment "
                    "(effective rate %u FPS, slow-frame threshold %llu us, drain 500 ms)",
                    m_PresentStallDetector->effectiveFps(),
                    static_cast<unsigned long long>(m_PresentStallDetector->thresholdUs()));
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    if (m_PresentStallDetector != nullptr) {
        if (m_PresentStallDetector->shouldSkipFrame(LiGetMicroseconds())) {
            {
                QMutexLocker locker(&m_FrameQueueLock);
                updateRenderQueueHistoryLocked();
            }
            ++m_DrainSkippedFrames;
            m_Stats->recordDrop();
            av_frame_free(&frame);
            return;
        }
        if (m_PresentStallDetector->takeDrainEnded()) {
            m_PostDrainCollectionPending = true;
        }
    }

    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();

    // Render it
    {
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
        auto surfaceLock = WaylandQtSdlBridge::lockSurfaceForRendering(m_Window);
#endif
        m_VsyncRenderer->renderFrame(frame);
    }
    uint64_t afterRender = LiGetMicroseconds();
    const uint64_t renderUs = afterRender - beforeRender;
    IFFmpegRenderer::RenderTiming renderTiming;
    const bool hasRenderTiming = m_VsyncRenderer->takeRenderTiming(&renderTiming);

    if (hasRenderTiming) {
        m_Stats->recordPresent(beforeRender - static_cast<uint64_t>(frame->pkt_dts), renderUs,
                               renderTiming.prePresentUs, renderTiming.presentUs);
    } else {
        m_Stats->recordPresent(beforeRender - static_cast<uint64_t>(frame->pkt_dts), renderUs);
        if (m_PresentStallDetector != nullptr) {
            m_PresentStallDetector->resetStreak();
        }
    }

    // Present times feed the frame time percentiles (1% and 0.1% lows)
    Session::get()->getStatsModel().recordFramePresented(afterRender);

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    QMutexLocker frameQueueLocker(&m_FrameQueueLock);

    int frameDropTarget;
    int queuedFrames = 0;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        updateRenderQueueHistoryLocked();
    }

    queuedFrames = m_RenderQueue.count();
    bool drainStarted = false;
    if (hasRenderTiming && m_PresentStallDetector != nullptr) {
        drainStarted = m_PresentStallDetector->observe(
            afterRender, renderUs, static_cast<size_t>(queuedFrames), renderTiming.prePresentUs,
            renderTiming.presentUs);
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        frameQueueLocker.unlock();
        m_Stats->recordDrop();
        av_frame_free(&frame);
        frameQueueLocker.relock();
    }
    frameQueueLocker.unlock();

    if (drainStarted) {
        const uint64_t avgRenderUs = m_PresentStallDetector->triggerAverageRenderUs();
        const uint64_t avgPrePresentUs = m_PresentStallDetector->triggerAveragePrePresentUs();
        const uint64_t avgPresentUs = m_PresentStallDetector->triggerAveragePresentUs();
        const char* classification =
            avgPresentUs >= avgPrePresentUs ? "Present-dominant" : "pre-Present-dominant";
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "MOONLIGHT_PRESENT_DRAIN: one-shot drain started (%s); last %u queued slow frames "
            "averaged render=%llu us, pre-Present=%llu us, Present=%llu us; queue=%d, "
            "threshold=%llu us",
            classification, PresentStallDetector::RequiredSlowFrames,
            static_cast<unsigned long long>(avgRenderUs),
            static_cast<unsigned long long>(avgPrePresentUs),
            static_cast<unsigned long long>(avgPresentUs), queuedFrames,
            static_cast<unsigned long long>(m_PresentStallDetector->thresholdUs()));
    }

    if (m_PostDrainCollectionPending) {
        m_PostDrainCollectionPending = false;
        m_CollectingPostDrainTiming = true;
        m_PostDrainStartUs = afterRender;
        m_PostDrainFrames = 0;
        m_PostDrainTotalRenderUs = 0;
        m_PostDrainTimedFrames = 0;
        m_PostDrainTotalPrePresentUs = 0;
        m_PostDrainTotalPresentUs = 0;
    }

    if (m_CollectingPostDrainTiming) {
        ++m_PostDrainFrames;
        m_PostDrainTotalRenderUs += renderUs;
        if (hasRenderTiming) {
            ++m_PostDrainTimedFrames;
            m_PostDrainTotalPrePresentUs += renderTiming.prePresentUs;
            m_PostDrainTotalPresentUs += renderTiming.presentUs;
        }
        if (afterRender - m_PostDrainStartUs >= 2000000) {
            const uint64_t averageRenderUs =
                m_PostDrainFrames > 0 ? m_PostDrainTotalRenderUs / m_PostDrainFrames : 0;
            const uint64_t averagePrePresentUs =
                m_PostDrainTimedFrames > 0 ? m_PostDrainTotalPrePresentUs / m_PostDrainTimedFrames
                                           : 0;
            const uint64_t averagePresentUs =
                m_PostDrainTimedFrames > 0 ? m_PostDrainTotalPresentUs / m_PostDrainTimedFrames : 0;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "MOONLIGHT_PRESENT_DRAIN: one-shot drain ended; skipped %llu frames; "
                        "post-drain 2 s averages over %llu rendered / %llu timed frames: "
                        "render=%llu us, pre-Present=%llu us, Present=%llu us",
                        static_cast<unsigned long long>(m_DrainSkippedFrames),
                        static_cast<unsigned long long>(m_PostDrainFrames),
                        static_cast<unsigned long long>(m_PostDrainTimedFrames),
                        static_cast<unsigned long long>(averageRenderUs),
                        static_cast<unsigned long long>(averagePrePresentUs),
                        static_cast<unsigned long long>(averagePresentUs));
            m_CollectingPostDrainTiming = false;
        }
    }
}

void Pacer::updateRenderQueueHistoryLocked()
{
    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        return;
    }
    if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
        m_RenderQueueHistory.dequeue();
    }
    m_RenderQueueHistory.enqueue(m_RenderQueue.count());
}

// Called on the render thread with m_FrameQueueLock held and a frame queued.
// In VRR mode, holds that frame until the present-rate cap allows it. Frames
// arriving meanwhile stay queued; renderFrame() drops the excess if the stream
// stays above the cap.
void Pacer::waitForVrrPresentSlot()
{
    if (!m_VrrLimiter.isEnabled()) {
        return;
    }

    for (;;) {
        uint64_t waitUs = m_VrrLimiter.waitUs(LiGetMicroseconds());
        if (waitUs == 0 || m_Stopping) {
            break;
        }

        // Round up so the frame is never presented early. The destructor's
        // wakeAll() ends the wait on shutdown.
        m_RenderQueueNotEmpty.wait(&m_FrameQueueLock, (unsigned long)((waitUs + 999) / 1000));
    }

    m_VrrLimiter.onPresented(LiGetMicroseconds());
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        m_Stats->recordDrop();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
