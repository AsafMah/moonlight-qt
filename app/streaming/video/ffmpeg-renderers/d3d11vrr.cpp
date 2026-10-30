// Nonary VRR presenter for D3D11VARenderer (DXGI path).
//
// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master"),
// app/streaming/video/ffmpeg-renderers/d3d11va.cpp.
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: moved the IVrrFramePresenter
// implementation into its own file; removed the composition-swapchain
// presenter (MOONLIGHT_VRR_COMPOSITION) and PyroWave branches; gated on the
// fork's VRR implementation selection (m_NonaryVrrRequested) instead of
// enableVrr; prepare reuses the fork's renderFrame() pipeline.

#include "d3d11va.h"
#include "d3d11fencewait.h"
#include "windowsvblankvirtualization.h"

#include <SDL_syswm.h>
#include <Limelight.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool isBorderlessFullscreenWindow(SDL_Window* window)
{
    if (window == nullptr) {
        return false;
    }

    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
        SDL_WINDOW_FULLSCREEN_DESKTOP;
}

uint64_t packLuid(const LUID& luid)
{
    return static_cast<uint64_t>(luid.LowPart) |
        (static_cast<uint64_t>(
             static_cast<uint32_t>(luid.HighPart)) << 32);
}

struct VrrQpcClockCorrelation {
    LONGLONG referenceQpc = 0;
    LONGLONG qpcFrequency = 0;
    uint64_t referenceTimeUs = 0;
    uint64_t bracketSpanTicks = 0;
    bool valid = false;
};

const VrrQpcClockCorrelation& vrrQpcClockCorrelation()
{
    static const VrrQpcClockCorrelation correlation = []() {
        VrrQpcClockCorrelation value;
        // Ensure the Limelight clock origin is established before bracketing
        // the QPC sample used for the stable cross-clock correlation.
        (void)LiGetMicroseconds();
        LARGE_INTEGER frequency;
        LARGE_INTEGER before;
        LARGE_INTEGER after;
        if (!QueryPerformanceFrequency(&frequency) ||
                frequency.QuadPart <= 0) {
            return value;
        }
        // Retain the native frequency even if the cross-clock correlation
        // cannot be completed. A successful DXGI statistics query can then
        // still emit independently auditable raw SyncQPCTime evidence.
        value.qpcFrequency = frequency.QuadPart;
        if (!QueryPerformanceCounter(&before)) {
            return value;
        }
        value.referenceTimeUs = LiGetMicroseconds();
        if (!QueryPerformanceCounter(&after) ||
                after.QuadPart < before.QuadPart) {
            return value;
        }
        value.bracketSpanTicks = static_cast<uint64_t>(
            after.QuadPart - before.QuadPart);
        value.referenceQpc = before.QuadPart +
            (after.QuadPart - before.QuadPart) / 2;
        value.valid = true;
        return value;
    }();
    return correlation;
}

bool translateVrrSyncQpcTime(LARGE_INTEGER syncQpcTime,
                             uint64_t& translatedTimeUs,
                             uint64_t& qpcFrequency)
{
    translatedTimeUs = 0;
    qpcFrequency = 0;
    const VrrQpcClockCorrelation& correlation =
        vrrQpcClockCorrelation();
    if (correlation.qpcFrequency > 0) {
        qpcFrequency = static_cast<uint64_t>(correlation.qpcFrequency);
    }
    if (!correlation.valid) {
        return false;
    }
    if (syncQpcTime.QuadPart <= 0) {
        return false;
    }

    const uint64_t deltaTicks = syncQpcTime.QuadPart >=
            correlation.referenceQpc ?
        static_cast<uint64_t>(
            syncQpcTime.QuadPart - correlation.referenceQpc) :
        static_cast<uint64_t>(
            correlation.referenceQpc - syncQpcTime.QuadPart);
    const uint64_t frequency = static_cast<uint64_t>(
        correlation.qpcFrequency);
    const uint64_t wholeSeconds = deltaTicks / frequency;
    const uint64_t remainderTicks = deltaTicks % frequency;
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    if (wholeSeconds > maximum / 1000000ULL ||
            remainderTicks > maximum / 1000000ULL) {
        return false;
    }
    const uint64_t wholeUs = wholeSeconds * 1000000ULL;
    const uint64_t remainderUs =
        remainderTicks * 1000000ULL / frequency;
    if (remainderUs > maximum - wholeUs) {
        return false;
    }
    const uint64_t deltaUs = wholeUs + remainderUs;
    if (syncQpcTime.QuadPart < correlation.referenceQpc) {
        if (deltaUs > correlation.referenceTimeUs) {
            return false;
        }
        translatedTimeUs = correlation.referenceTimeUs - deltaUs;
    }
    else {
        if (deltaUs > maximum - correlation.referenceTimeUs) {
            return false;
        }
        translatedTimeUs = correlation.referenceTimeUs + deltaUs;
    }
    return true;
}

}

uint64_t D3D11VARenderer::captureDecodeBoundary()
{
    if (m_DecodeDevice == m_RenderDevice ||
            m_DecodeDeviceContext == nullptr ||
            m_DecodeD2RFence == nullptr) {
        return 0;
    }

    // This runs at decoder output, before a successor can enqueue more decode
    // commands. Preserve that exact GPU boundary so rendering this frame does
    // not accidentally wait for every newer decode already in flight.
    lockContext(this);
    const UINT64 fenceValue = m_D2RFenceValue++;
    const HRESULT hr = m_DecodeDeviceContext->Signal(
        m_DecodeD2RFence.Get(), fenceValue);
    if (SUCCEEDED(hr)) {
        // Signal belongs to the decoder's immediate context. Flushing the
        // render context cannot dispatch it. Submit the boundary now so the
        // GPU-side render wait can progress without another compressed frame
        // arriving to flush decoder commands. Flush submits; it does not wait
        // for decode completion on the CPU.
        m_DecodeDeviceContext->Flush();
    }
    unlockContext(this);

    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR decode-boundary Signal() failed: %x", hr);
        return 0;
    }

    return fenceValue;
}

uint64_t D3D11VARenderer::waitForDecode(AVFrame* frame, uint64_t decodeBoundary)
{
    return observeDecodeReadiness(frame, decodeBoundary).waitUs;
}

VrrDecodeReadiness D3D11VARenderer::observeDecodeReadiness(AVFrame* frame, uint64_t decodeBoundary)
{
    Q_UNUSED(frame);

    if (decodeBoundary == 0 || m_DecodeD2RFence == nullptr) {
        return {0, false, true};
    }

    const uint64_t startUs = LiGetMicroseconds();
    const auto completed = m_DecodeD2RFence->GetCompletedValue();
    if (completed == (std::numeric_limits<UINT64>::max)()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "D3D11 VRR decode-ready fence reported device removal (target=%llu device=%x)",
            static_cast<unsigned long long>(decodeBoundary),
            m_DecodeDevice->GetDeviceRemovedReason());
        m_VrrPresentReadyAvailable = false;
        m_VrrFallbackReason = VrrFallbackReason::AdaptivePresentationUnavailable;
        queueRenderDeviceReset();
        return {0, false, true};
    }
    if (completed >= decodeBoundary) {
        return {0, true, true};
    }

    // renderVideo() still queues an ID3D11DeviceContext4::Wait for this
    // boundary, which is the correctness mechanism. This CPU wait supplies
    // the decode-completion observation that production source mapping is
    // anchored to, as vaSyncSurface() does on Linux. Without it the mapping
    // falls back to decoder output, which precedes the hardware decode, and
    // the whole decode duration must fit inside the capped playout buffer.
    // Only a monitored fence reports GPU progress to the CPU.
    if (m_FenceType != SupportedFenceType::Monitored ||
            m_VrrDecodeReadyEvent == nullptr) {
        return {0, false, true};
    }

    // The decoder thread already signalled and flushed this value. Fence
    // completion needs neither immediate context, so take no context lock:
    // FFmpeg keeps submitting the next frame while this worker waits.
    const HRESULT eventResult = m_DecodeD2RFence->SetEventOnCompletion(
        decodeBoundary, m_VrrDecodeReadyEvent);
    DWORD lastEventResult = WAIT_TIMEOUT;
    const auto result = D3D11FenceWait::wait(decodeBoundary, LiGetMicroseconds,
        [&] { return m_DecodeD2RFence->GetCompletedValue(); },
        [&](unsigned timeoutMs) {
            if (FAILED(eventResult)) {
                Sleep(timeoutMs);
                return true;
            }
            lastEventResult = WaitForSingleObject(m_VrrDecodeReadyEvent, timeoutMs);
            return lastEventResult == WAIT_OBJECT_0 || lastEventResult == WAIT_TIMEOUT;
        });
    if (result.status != D3D11FenceWait::Status::Complete) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "D3D11 VRR decode-ready fence wait failed (target=%llu completed=%llu device=%x)",
            static_cast<unsigned long long>(decodeBoundary),
            static_cast<unsigned long long>(result.completedValue),
            m_DecodeDevice->GetDeviceRemovedReason());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "D3D11 VRR decode-ready wait detail: stop=%s elapsed_us=%llu wait_calls=%u event_setup=%x event=%lu render_device=%x",
            D3D11FenceWait::stopReasonName(result.stopReason),
            static_cast<unsigned long long>(result.elapsedUs), result.waitCalls,
            eventResult, static_cast<unsigned long>(lastEventResult),
            m_RenderDevice->GetDeviceRemovedReason());
        m_VrrPresentReadyAvailable = false;
        m_VrrFallbackReason = VrrFallbackReason::AdaptivePresentationUnavailable;
        queueRenderDeviceReset();
        return {0, false, true}; // Failure must never advertise GPU readiness.
    }
    return {LiGetMicroseconds() - startUs, true, true};
}


void D3D11VARenderer::lockPresentation()
{
    SDL_LockMutex(m_PresentationLock);

    // A shared device has one immediate context for decoding and rendering,
    // so rendering must also exclude FFmpeg. Separate devices must not take
    // FFmpeg's lock here: holding it across Present serialized decode
    // submission behind presentation, and the collisions line up with the
    // cadence because the next frame's submission lands near this frame's
    // target. renderVideo() locks the decode context only for its own calls.
    if (m_DecodeDevice == m_RenderDevice) {
        lockContext(this);
    }
}

void D3D11VARenderer::unlockPresentation()
{
    if (m_DecodeDevice == m_RenderDevice) {
        unlockContext(this);
    }

    SDL_UnlockMutex(m_PresentationLock);
}

void D3D11VARenderer::initializeVrrPresentationState(SDL_Window* window,
                                                       DXGI_SWAP_CHAIN_DESC1* swapChainDesc)
{
    m_AllowTearing = false;
    m_VrrTearingSupported = false;
    m_VrrBorderlessFlipModel = false;
    m_VrrSameGpuOutput = false;
    m_VrrSwapChainAllowsTearing = false;
    m_VrrTearingFeatureQueryResultValid = false;
    m_VrrTearingFeatureQueryResult = 0;
    m_VrrTearingFeatureAllowsTearing = false;
    m_VrrSwapChainDescQueryResultValid = false;
    m_VrrSwapChainDescQueryResult = 0;
    m_VrrSwapChainFlags = 0;
    m_VrrSwapChainSwapEffect = 0;
    m_VrrFullscreenStateQueryResultValid = false;
    m_VrrFullscreenStateQueryResult = 0;
    m_VrrFullscreenExclusive = false;
    m_VrrWindowFlags = window != nullptr ?
        SDL_GetWindowFlags(window) : 0;
    m_VrrDesktopMonitorCount = static_cast<UINT>(
        (std::max)(0, GetSystemMetrics(SM_CMONITORS)));
    m_VrrWindowHandle = nullptr;
    m_VrrDisplayTiming = {};
    closeVrrRasterSource();
    const char* rasterSamplingEnv = SDL_getenv("MOONLIGHT_VRR_ALIGN");
    m_VrrRasterSamplingRequested =
        rasterSamplingEnv != nullptr &&
        rasterSamplingEnv[0] == '1' &&
        rasterSamplingEnv[1] == '\0';
    const char* syncFlipsEnv = SDL_getenv("MOONLIGHT_VRR_SYNC_FLIPS");
    m_VrrSyncFlips = syncFlipsEnv != nullptr &&
        syncFlipsEnv[0] == '1' && syncFlipsEnv[1] == '\0';
    SDL_SysWMinfo windowInfo;
    SDL_VERSION(&windowInfo.version);
    if (window != nullptr &&
            SDL_GetWindowWMInfo(window, &windowInfo) &&
            windowInfo.subsystem == SDL_SYSWM_WINDOWS) {
        m_VrrWindowHandle = windowInfo.info.win.window;
    }
    m_VrrSuspended = false;
    m_VrrFallbackReason = VrrFallbackReason::NoFallback;
    m_VrrPresentReadyAvailable = false;
    m_VrrPriorPresentCountValid = false;
    m_VrrPriorPresentCount = 0;
    m_VrrPriorFrameStatsValid = false;
    m_VrrPriorFrameStatsPresentCount = 0;
    m_VrrPriorFrameStatsTimeUs = 0;
    m_VrrPriorFrameStatsPresentRefreshSequence = 0;
    m_VrrPriorFrameStatsRefreshSequence = 0;

    if (m_NonaryVrrRequested) {
        // Prime the process-wide correlation and display snapshot during
        // renderer setup so the first deeply traced Present does not pay
        // one-time initialization cost on the pacing thread.
        (void)vrrQpcClockCorrelation();
        refreshVrrDisplayTiming();
    }

    // Preserve the legacy non-VSync path while also creating an
    // allow-tearing flip swapchain for an explicitly requested VRR session.
    // The latter is required even though the session's effective V-Sync is
    // true: the VRR worker always uses immediate presentation and owns the
    // complete mathematical pacing policy.
    if (!m_DecoderParams.enableVsync || m_NonaryVrrRequested) {
        BOOL allowTearing = FALSE;
        HRESULT hr = m_Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allowTearing,
                                                    sizeof(allowTearing));
        m_VrrTearingFeatureQueryResultValid = true;
        m_VrrTearingFeatureQueryResult = static_cast<int64_t>(hr);
        m_VrrTearingFeatureAllowsTearing = allowTearing != FALSE;
        if (SUCCEEDED(hr) && allowTearing) {
            m_VrrTearingSupported = true;
            swapChainDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            m_VrrSwapChainAllowsTearing = true;

            // Do not alter legacy V-Sync semantics.  Only the existing
            // non-VSync path uses this flag from renderFrame().
            if (!m_DecoderParams.enableVsync) {
                m_AllowTearing = true;
            }
        }
        else if (!m_DecoderParams.enableVsync) {
            if (SUCCEEDED(hr)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "OS/GPU doesn't support DXGI_FEATURE_PRESENT_ALLOW_TEARING");
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "IDXGIFactory::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) failed: %x",
                             hr);
            }
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: DXGI_FEATURE_PRESENT_ALLOW_TEARING is unavailable (%x)",
                        hr);
        }
    }

    if (!m_NonaryVrrRequested) {
        return;
    }

    m_VrrPresentReadyAvailable = initializeVrrPresentReadyFence();
    if (!m_VrrPresentReadyAvailable) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: GPU present-ready fencing is unavailable");
    }

    m_VrrBorderlessFlipModel = isBorderlessFullscreenWindow(window) &&
        (swapChainDesc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
         swapChainDesc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    m_VrrSameGpuOutput = m_RenderAdapterIndex >= 0 &&
        m_RenderAdapterIndex == m_AdapterIndex;
    m_VrrFallbackReason = evaluateVrrEligibility(false);
    switch (m_VrrFallbackReason) {
    case VrrFallbackReason::NoFallback:
        break;
    case VrrFallbackReason::IneffectiveVsync:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR unavailable: effective V-sync is disabled");
        break;
    case VrrFallbackReason::UnsupportedRenderer:
        if (!m_VrrBorderlessFlipModel) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: a borderless flip-model swapchain is required");
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: render adapter %d does not own output adapter %d",
                        m_RenderAdapterIndex,
                        m_AdapterIndex);
        }
        break;
    case VrrFallbackReason::InvalidRefresh:
        // Session already performs the strict query exactly once and preserves
        // the result for every renderer recreation. Do not silently substitute
        // a new display value or the legacy 60 Hz fallback here.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: the active display has no valid refresh rate");
        break;
    case VrrFallbackReason::MainThreadRenderer:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: the renderer cannot stay on the pacing worker thread");
        break;
    case VrrFallbackReason::AdaptivePresentationUnavailable:
        if (!m_VrrPresentReadyAvailable) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: GPU present-ready fencing is unavailable");
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR unavailable: DXGI tearing support is unavailable");
        }
        break;
    case VrrFallbackReason::InitializationFailed:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: the flip-model swapchain lacks DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING");
        break;
    default:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR unavailable: %s",
                    vrrFallbackReasonName(m_VrrFallbackReason));
        break;
    }
}

void D3D11VARenderer::refreshVrrDisplayState()
{
    if (!m_NonaryVrrRequested) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    HRESULT swapChainDescResult = E_POINTER;
    if (m_SwapChain != nullptr) {
        swapChainDescResult = m_SwapChain->GetDesc1(&swapChainDesc);
    }
    m_VrrSwapChainDescQueryResultValid = m_SwapChain != nullptr;
    m_VrrSwapChainDescQueryResult =
        static_cast<int64_t>(swapChainDescResult);
    const bool gotSwapChainDesc = swapChainDescResult == S_OK;
    m_VrrSwapChainFlags =
        gotSwapChainDesc ? swapChainDesc.Flags : 0;
    m_VrrSwapChainSwapEffect =
        gotSwapChainDesc ?
            static_cast<uint32_t>(swapChainDesc.SwapEffect) : 0;

    BOOL fullscreenExclusive = FALSE;
    HRESULT fullscreenStateResult = E_POINTER;
    if (m_SwapChain != nullptr) {
        fullscreenStateResult = m_SwapChain->GetFullscreenState(
            &fullscreenExclusive, nullptr);
    }
    m_VrrFullscreenStateQueryResultValid = m_SwapChain != nullptr;
    m_VrrFullscreenStateQueryResult =
        static_cast<int64_t>(fullscreenStateResult);
    m_VrrFullscreenExclusive =
        fullscreenStateResult == S_OK &&
        fullscreenExclusive != FALSE;
    m_VrrWindowFlags = m_DecoderParams.window != nullptr ?
        SDL_GetWindowFlags(m_DecoderParams.window) : 0;
    m_VrrBorderlessFlipModel = gotSwapChainDesc &&
        fullscreenStateResult == S_OK &&
        !m_VrrFullscreenExclusive &&
        (m_VrrWindowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
            SDL_WINDOW_FULLSCREEN_DESKTOP &&
        (swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
         swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    m_VrrSwapChainAllowsTearing = gotSwapChainDesc &&
        (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    m_VrrSameGpuOutput = m_RenderAdapterIndex >= 0 &&
        m_RenderAdapterIndex == m_AdapterIndex;
    m_VrrDesktopMonitorCount = static_cast<UINT>(
        (std::max)(0, GetSystemMetrics(SM_CMONITORS)));
    refreshVrrDisplayTiming();

    VrrFallbackReason previousReason = m_VrrFallbackReason;
    // Keep the established display-update precedence: an output mismatch is
    // reported before a stale refresh snapshot, whereas initial setup logs the
    // stricter refresh qualification first.
    m_VrrFallbackReason = evaluateVrrEligibility(true);

    if (m_VrrFallbackReason != previousReason) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR display update: %s",
                    vrrFallbackReasonName(m_VrrFallbackReason));
    }
}

VrrFallbackReason D3D11VARenderer::evaluateVrrEligibility(
    bool prioritizeOutputCompatibility)
{
    if (!m_DecoderParams.enableVsync) {
        return VrrFallbackReason::IneffectiveVsync;
    }
    if (!m_VrrBorderlessFlipModel ||
        (prioritizeOutputCompatibility && !m_VrrSameGpuOutput)) {
        return VrrFallbackReason::UnsupportedRenderer;
    }
    if (m_DecoderParams.vrrDisplayRefreshHz <= 0) {
        return VrrFallbackReason::InvalidRefresh;
    }
    if (!m_VrrSameGpuOutput) {
        return VrrFallbackReason::UnsupportedRenderer;
    }
    if (!isRenderThreadSupported()) {
        return VrrFallbackReason::MainThreadRenderer;
    }
    if (!m_VrrPresentReadyAvailable) {
        return VrrFallbackReason::AdaptivePresentationUnavailable;
    }
    if (!m_VrrTearingSupported) {
        return VrrFallbackReason::AdaptivePresentationUnavailable;
    }
    if (!m_VrrSwapChainAllowsTearing) {
        return VrrFallbackReason::InitializationFailed;
    }

    return VrrFallbackReason::NoFallback;
}

void D3D11VARenderer::releasePreparedVrrFrame()
{
    // Present() unbinds the render target itself.  A cancellation does not,
    // so explicitly remove the context's reference to the back buffer before
    // a resize/device reset can tear it down.
    if ((m_VrrFramePrepared || m_VrrPresentationLocked) &&
            m_RenderDeviceContext != nullptr) {
        m_RenderDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    }

    m_VrrFramePrepared = false;
    m_VrrPreparedCompletion.clear();
    m_VrrPreparedDecodeBoundary = 0;
    m_VrrGpuReadyAttempted = false;
    m_VrrGpuReadySignalResultValid = false;
    m_VrrGpuReadySignalResult = 0;
    m_VrrGpuReadySetEventResultValid = false;
    m_VrrGpuReadySetEventResult = 0;
    m_VrrGpuReadyWaitResultValid = false;
    m_VrrGpuReadyWaitResult = 0;
    m_VrrGpuReadyTimingValid = false;
    m_VrrGpuReadySignalStartUs = 0;
    m_VrrGpuReadySignalEndUs = 0;
    m_VrrGpuReadyFlushStartUs = 0;
    m_VrrGpuReadyFlushEndUs = 0;
    m_VrrGpuReadySetEventStartUs = 0;
    m_VrrGpuReadySetEventEndUs = 0;
    m_VrrGpuReadyPollStartUs = 0;
    m_VrrGpuReadyPollEndUs = 0;
    m_VrrGpuReadyFenceValue = 0;
    m_VrrGpuReadyPollCompletedValue = 0;
    m_VrrGpuReadyCompletedBeforeWait = false;
    m_VrrGpuReadyWaitStartUs = 0;
    m_VrrGpuReadyTimeUs = 0;

    if (m_VrrPresentationLocked) {
        // Publish that this path no longer owns the mutex before releasing
        // it. A window callback can acquire the same mutex immediately after
        // unlock and must never mistake its own ownership for ours.
        m_VrrPresentationLocked = false;
        unlockPresentation();
    }
}

bool D3D11VARenderer::retirePreparedVrrFrameForMutation()
{
    if (!m_VrrFramePrepared) {
        return true;
    }

    // ResizeBuffers and composition/display replacement may not invalidate a
    // back buffer while this frame's GPU writes are outstanding. These state
    // changes are rare and already hold the presentation mutex, so drain the
    // bounded fence without releasing that mutex. A normally prepared frame
    // already has verified completion, so this becomes a state check.
    const bool completed = finishVrrPresentReady(false);
    if (!completed) {
        m_VrrFallbackReason =
            VrrFallbackReason::AdaptivePresentationUnavailable;
    }
    releasePreparedVrrFrame();
    if (!completed) {
        queueRenderDeviceReset();
    }
    return completed;
}

void D3D11VARenderer::populateVrrGpuReadyFeedback(
    VrrPresentFeedback& feedback) const
{
    feedback.gpuReadyAttempted = m_VrrGpuReadyAttempted;
    feedback.gpuReadySignalResultValid =
        m_VrrGpuReadySignalResultValid;
    feedback.gpuReadySignalResult = m_VrrGpuReadySignalResult;
    feedback.gpuReadySetEventResultValid =
        m_VrrGpuReadySetEventResultValid;
    feedback.gpuReadySetEventResult = m_VrrGpuReadySetEventResult;
    feedback.gpuReadyWaitResultValid =
        m_VrrGpuReadyWaitResultValid;
    feedback.gpuReadyWaitResult = m_VrrGpuReadyWaitResult;
    feedback.gpuReadyTimingValid = m_VrrGpuReadyTimingValid;
    feedback.gpuReadySignalStartUs = m_VrrGpuReadySignalStartUs;
    feedback.gpuReadySignalEndUs = m_VrrGpuReadySignalEndUs;
    feedback.gpuReadyFlushStartUs = m_VrrGpuReadyFlushStartUs;
    feedback.gpuReadyFlushEndUs = m_VrrGpuReadyFlushEndUs;
    feedback.gpuReadySetEventStartUs =
        m_VrrGpuReadySetEventStartUs;
    feedback.gpuReadySetEventEndUs =
        m_VrrGpuReadySetEventEndUs;
    feedback.gpuReadyPollStartUs = m_VrrGpuReadyPollStartUs;
    feedback.gpuReadyPollEndUs = m_VrrGpuReadyPollEndUs;
    feedback.gpuReadyFenceValue = m_VrrGpuReadyFenceValue;
    feedback.gpuReadyPollCompletedValue =
        m_VrrGpuReadyPollCompletedValue;
    feedback.gpuReadyCompletedBeforeWait =
        m_VrrGpuReadyCompletedBeforeWait;
    feedback.gpuReadyWaitStartUs = m_VrrGpuReadyWaitStartUs;
    feedback.gpuReadyTimeUs = m_VrrGpuReadyTimeUs;
}

void D3D11VARenderer::queueRenderDeviceReset()
{
    SDL_Event event = {};
    event.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
}


bool D3D11VARenderer::initializeVrrPresentReadyFence()
{
    m_VrrPresentReadyFence.Reset();
    if (m_VrrPresentReadyFenceEvent != nullptr) {
        CloseHandle(m_VrrPresentReadyFenceEvent);
        m_VrrPresentReadyFenceEvent = nullptr;
    }
    m_VrrPresentReadyFenceValue = 0;

    // waitForDecode() keeps its own wake event so a stale present-ready
    // notification never costs the decode wait an extra poll, and vice versa.
    // Without it, the decode wait stays GPU-side only.
    if (m_VrrDecodeReadyEvent == nullptr) {
        m_VrrDecodeReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_VrrDecodeReadyEvent == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "D3D11 VRR could not create the decode-ready event: %lu",
                        GetLastError());
        }
    }

    HRESULT hr = m_RenderDevice->CreateFence(
        0, D3D11_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_VrrPresentReadyFence));
    if (SUCCEEDED(hr)) {
        m_VrrPresentReadyFenceEvent = CreateEventW(
            nullptr, FALSE, FALSE, nullptr);
    }

    if (FAILED(hr) || m_VrrPresentReadyFenceEvent == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR could not create the present-ready fence: %x",
                    hr);
        m_VrrPresentReadyFence.Reset();
        if (m_VrrPresentReadyFenceEvent != nullptr) {
            CloseHandle(m_VrrPresentReadyFenceEvent);
            m_VrrPresentReadyFenceEvent = nullptr;
        }
        return false;
    }

    return true;
}

bool D3D11VARenderer::beginVrrPresentReady()
{
    m_VrrGpuReadyAttempted = false;
    m_VrrGpuReadySignalResultValid = false;
    m_VrrGpuReadySignalResult = 0;
    m_VrrGpuReadySetEventResultValid = false;
    m_VrrGpuReadySetEventResult = 0;
    m_VrrGpuReadyWaitResultValid = false;
    m_VrrGpuReadyWaitResult = 0;
    m_VrrGpuReadyTimingValid = false;
    m_VrrGpuReadySignalStartUs = 0;
    m_VrrGpuReadySignalEndUs = 0;
    m_VrrGpuReadyFlushStartUs = 0;
    m_VrrGpuReadyFlushEndUs = 0;
    m_VrrGpuReadySetEventStartUs = 0;
    m_VrrGpuReadySetEventEndUs = 0;
    m_VrrGpuReadyPollStartUs = 0;
    m_VrrGpuReadyPollEndUs = 0;
    m_VrrGpuReadyFenceValue = 0;
    m_VrrGpuReadyPollCompletedValue = 0;
    m_VrrGpuReadyCompletedBeforeWait = false;
    m_VrrGpuReadyWaitStartUs = 0;
    m_VrrGpuReadyTimeUs = 0;
    if (!m_VrrPresentReadyAvailable ||
            m_VrrPresentReadyFence == nullptr ||
            m_VrrPresentReadyFenceEvent == nullptr) {
        return false;
    }

    // Queue a completion marker immediately after this frame's rendering.
    // The pacing worker will spend any remaining lead time waiting for its
    // cadence target after prepareFrame() has verified this exact value.
    // No deferred GPU wait may move the native Present past that target.
    const UINT64 fenceValue = ++m_VrrPresentReadyFenceValue;
    m_VrrPreparedCompletion.begin(fenceValue);
    m_VrrGpuReadyAttempted = true;
    m_VrrGpuReadyFenceValue = fenceValue;
    m_VrrGpuReadySignalStartUs = LiGetMicroseconds();
    HRESULT hr = m_RenderDeviceContext->Signal(
        m_VrrPresentReadyFence.Get(), fenceValue);
    m_VrrGpuReadySignalEndUs = LiGetMicroseconds();
    m_VrrGpuReadySignalResultValid = true;
    m_VrrGpuReadySignalResult = static_cast<int64_t>(hr);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready Signal() failed: %x", hr);
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    m_VrrGpuReadyFlushStartUs = LiGetMicroseconds();
    m_RenderDeviceContext->Flush();
    m_VrrGpuReadyFlushEndUs = LiGetMicroseconds();
    m_VrrGpuReadySetEventStartUs = LiGetMicroseconds();
    hr = m_VrrPresentReadyFence->SetEventOnCompletion(
        fenceValue, m_VrrPresentReadyFenceEvent);
    m_VrrGpuReadySetEventEndUs = LiGetMicroseconds();
    m_VrrGpuReadySetEventResultValid = true;
    m_VrrGpuReadySetEventResult = static_cast<int64_t>(hr);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready SetEventOnCompletion() failed: %x",
                     hr);
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    // GetCompletedValue() is a nonblocking observation. If the target value
    // is still incomplete, its call start is a conservative lower bound for
    // the eventual completion. If it is already complete, Signal() call start
    // remains the only defensible lower bound and this poll end is the upper
    // bound. The preparation completion wait supplies the completion upper
    // bound when rendering is still outstanding here.
    m_VrrGpuReadyPollStartUs = LiGetMicroseconds();
    const UINT64 completedValue =
        m_VrrPresentReadyFence->GetCompletedValue();
    m_VrrGpuReadyPollCompletedValue = completedValue;
    m_VrrGpuReadyPollEndUs = LiGetMicroseconds();
    if (completedValue == (std::numeric_limits<UINT64>::max)()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready fence reported device removal after Signal() (target=%llu device=%x)",
                     static_cast<unsigned long long>(fenceValue),
                     m_RenderDevice->GetDeviceRemovedReason());
        m_VrrPresentReadyAvailable = false;
        return false;
    }
    m_VrrGpuReadyCompletedBeforeWait = completedValue >= fenceValue;
    return true;
}

bool D3D11VARenderer::finishVrrPresentReady(
    bool releasePresentationWhileWaiting)
{
    if (m_VrrPreparedCompletion.ready(m_VrrGpuReadyFenceValue)) {
        return true;
    }
    if (!m_VrrGpuReadyAttempted ||
            !m_VrrGpuReadySignalResultValid ||
            FAILED(static_cast<HRESULT>(m_VrrGpuReadySignalResult)) ||
            !m_VrrGpuReadySetEventResultValid ||
            FAILED(static_cast<HRESULT>(m_VrrGpuReadySetEventResult)) ||
            m_VrrPresentReadyFence == nullptr ||
            m_VrrPresentReadyFenceEvent == nullptr ||
            m_VrrGpuReadyFenceValue == 0) {
        return false;
    }

    const UINT64 fenceValue = m_VrrGpuReadyFenceValue;
    const ComPtr<ID3D11Fence> fence = m_VrrPresentReadyFence;
    const HANDLE fenceEvent = m_VrrPresentReadyFenceEvent;

    // Once the completion marker has been submitted, checking it needs
    // neither immediate context. Release presentation (and, on a shared
    // device, FFmpeg's lock with it) so window changes and decoding can
    // continue while rendering completes before the worker's target hold.
    const bool presentationReleased =
        releasePresentationWhileWaiting && m_VrrPresentationLocked;
    if (presentationReleased) {
        m_VrrPresentationLocked = false;
        unlockPresentation();
    }

    // Keep every wait result local while the mutex is released. A UI
    // display/resize callback may cancel the prepared frame in that interval;
    // it resets the member telemetry under the same mutex. Publishing after
    // reacquisition avoids racing that reset or attaching this completion to
    // a replacement frame.
    const uint64_t waitStartUs = LiGetMicroseconds();
    DWORD lastEventResult = WAIT_TIMEOUT;
    uint64_t initialPollStartUs = 0, initialPollEndUs = 0, initialCompletedValue = 0;
    bool sampledInitialPoll = false;
    const auto fenceWait = D3D11FenceWait::wait(fenceValue, LiGetMicroseconds,
        [&] {
            const auto pollStartUs = LiGetMicroseconds();
            const auto value = fence->GetCompletedValue();
            if (!sampledInitialPoll) {
                initialPollStartUs = pollStartUs;
                initialPollEndUs = LiGetMicroseconds();
                initialCompletedValue = value;
                sampledInitialPoll = true;
            }
            return value;
        },
        [&](unsigned timeoutMs) {
            lastEventResult = WaitForSingleObject(fenceEvent, timeoutMs);
            return lastEventResult == WAIT_OBJECT_0 || lastEventResult == WAIT_TIMEOUT;
        });
    // This is the result of the complete fence wait, including completion
    // polling. An individual event timeout is not a fence timeout.
    const DWORD waitResult = fenceWait.status == D3D11FenceWait::Status::Complete ? WAIT_OBJECT_0 :
        fenceWait.status == D3D11FenceWait::Status::Timeout ? WAIT_TIMEOUT : WAIT_FAILED;
    const uint64_t readyTimeUs = LiGetMicroseconds();

    if (presentationReleased) {
        lockPresentation();
        m_VrrPresentationLocked = true;
    }

    if (!m_VrrFramePrepared || m_VrrGpuReadyFenceValue != fenceValue) {
        // A window-state callback cancelled or replaced the frame while this
        // thread was outside the presentation mutex. Its reset is authoritative.
        return false;
    }

    m_VrrGpuReadyWaitStartUs = initialPollEndUs;
    // Readiness describes the exact completion check during preparation,
    // rather than the earlier poll immediately after submitting the marker.
    m_VrrGpuReadyPollStartUs = initialPollStartUs;
    m_VrrGpuReadyPollEndUs = initialPollEndUs;
    m_VrrGpuReadyPollCompletedValue = initialCompletedValue;
    m_VrrGpuReadyCompletedBeforeWait =
        fenceWait.status == D3D11FenceWait::Status::Complete && fenceWait.waitCalls == 0;
    m_VrrGpuReadyWaitResultValid = true;
    m_VrrGpuReadyWaitResult = waitResult;
    m_VrrGpuReadyTimeUs = readyTimeUs;

    if (waitResult != WAIT_OBJECT_0) {
        const uint64_t lockReacquireUs = LiGetMicroseconds() - readyTimeUs;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR present-ready fence wait failed or timed out: %lu (target=%llu completed=%llu event=%lu device=%x)",
                     static_cast<unsigned long>(waitResult),
                     static_cast<unsigned long long>(fenceValue),
                     static_cast<unsigned long long>(fenceWait.completedValue),
                     static_cast<unsigned long>(lastEventResult),
                     m_RenderDevice->GetDeviceRemovedReason());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "D3D11 VRR present-ready wait detail: stop=%s elapsed_us=%llu wait_calls=%u signal_us=%llu flush_us=%llu event_setup_us=%llu lock_reacquire_us=%llu decode_device=%x separate=%d bind=%d frame_decode_target=%llu",
            D3D11FenceWait::stopReasonName(fenceWait.stopReason),
            static_cast<unsigned long long>(fenceWait.elapsedUs), fenceWait.waitCalls,
            static_cast<unsigned long long>(m_VrrGpuReadySignalEndUs - m_VrrGpuReadySignalStartUs),
            static_cast<unsigned long long>(m_VrrGpuReadyFlushEndUs - m_VrrGpuReadyFlushStartUs),
            static_cast<unsigned long long>(m_VrrGpuReadySetEventEndUs - m_VrrGpuReadySetEventStartUs),
            static_cast<unsigned long long>(lockReacquireUs),
            m_DecodeDevice->GetDeviceRemovedReason(), m_DecodeDevice != m_RenderDevice,
            m_BindDecoderOutputTextures,
            static_cast<unsigned long long>(m_VrrPreparedDecodeBoundary));
        if (m_DecodeDevice != m_RenderDevice) {
            // Failure-only snapshots, observed after reacquiring presentation.
            // These are not simultaneous GPU observations. A newer decode signal
            // may already be queued; next_signal is not this frame's dependency.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "D3D11 VRR fence snapshot: d2r_decode=%llu d2r_render=%llu d2r_next_signal=%llu r2d_render=%llu r2d_decode=%llu r2d_next_signal=%llu",
                static_cast<unsigned long long>(m_DecodeD2RFence->GetCompletedValue()),
                static_cast<unsigned long long>(m_RenderD2RFence->GetCompletedValue()),
                static_cast<unsigned long long>(m_D2RFenceValue),
                static_cast<unsigned long long>(m_RenderR2DFence->GetCompletedValue()),
                static_cast<unsigned long long>(m_DecodeR2DFence->GetCompletedValue()),
                static_cast<unsigned long long>(m_R2DFenceValue));
        }
        m_VrrPresentReadyAvailable = false;
        return false;
    }

    m_VrrGpuReadyTimingValid = true;
    m_VrrPreparedCompletion.complete(fenceValue, true);
    return true;
}

HRESULT D3D11VARenderer::presentPreparedFrame(
    const DxgiPresentParameters& parameters)
{
    if (m_SwapChain == nullptr) {
        return E_FAIL;
    }

    const HRESULT result = parameters.present(*m_SwapChain.Get());
    m_DxgiAdmission.presented();
    return result;
}


IVrrFramePresenter* D3D11VARenderer::getVrrFramePresenter()
{
    return this;
}

VrrFallbackReason D3D11VARenderer::checkSupport() const
{
    if (m_VrrFallbackReason != VrrFallbackReason::NoFallback) {
        return m_VrrFallbackReason;
    }

    return m_NonaryVrrRequested && m_SwapChain != nullptr &&
        m_VrrSwapChainAllowsTearing ? VrrFallbackReason::NoFallback :
        VrrFallbackReason::InitializationFailed;
}

VrrPrepareResult D3D11VARenderer::prepareFrame(AVFrame* frame,
                                               uint64_t decodeBoundary)
{
    VrrPrepareResult result;
    if (m_VrrSuspended || frame == nullptr) {
        return result;
    }

    // The contract guarantees one worker, but make an accidental second
    // preparation recoverable instead of leaking a retained presentation lock.
    if (m_VrrFramePrepared) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "D3D11 VRR discarded an unpresented prepared frame");
        result.feedback = cancelFrame();
        return result;
    }

    const auto admissionEpoch = m_DxgiAdmission.epoch();
    if (m_DxgiWaitableActive) {
        const auto admitted = m_DxgiAdmission.acquire(admissionEpoch,
            [] { return LiGetMicroseconds(); }, [this](unsigned timeoutMs) {
                const auto result = WaitForSingleObjectEx(m_DxgiFrameLatencyHandle.Get(), timeoutMs, FALSE);
                return result == WAIT_OBJECT_0 ? DxgiWaitable::Wake::Signalled :
                    result == WAIT_TIMEOUT ? DxgiWaitable::Wake::Timeout : DxgiWaitable::Wake::Failed;
            });
        if (admitted != DxgiWaitable::Status::Admitted) {
            if (admitted == DxgiWaitable::Status::Failed) queueRenderDeviceReset();
            if (admitted == DxgiWaitable::Status::Timeout) {
                const auto now = LiGetMicroseconds();
                if (m_DxgiWaitWarningUs == 0 || now - m_DxgiWaitWarningUs >= 1000000) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "DXGI queue admission timed out; skipping frame without rendering");
                    m_DxgiWaitWarningUs = now;
                }
            }
            return result;
        }
    }

    // Serialize rendering preparation with swap-chain state (and with decode
    // on a shared device). With separate devices, renderVideo() takes FFmpeg's
    // lock only around its own decode-context fence calls.
    lockPresentation();
    m_VrrPresentationLocked = true;

    if (m_DxgiWaitableActive && m_DxgiAdmission.interrupted(admissionEpoch)) {
        releasePreparedVrrFrame();
        return result;
    }

    // Display-state changes share this lock with preparation through Present.
    // Check eligibility only after taking it so a UI callback cannot replace
    // swapchain state concurrently.
    if (checkSupport() != VrrFallbackReason::NoFallback) {
        releasePreparedVrrFrame();
        return result;
    }

    if (!prepareFrameForPresent(frame, decodeBoundary)) {
        releasePreparedVrrFrame();
        return result;
    }

    m_VrrPreparedDecodeBoundary = decodeBoundary;
    if (!beginVrrPresentReady()) {
        m_VrrFallbackReason = VrrFallbackReason::AdaptivePresentationUnavailable;
        populateVrrGpuReadyFeedback(result.feedback);
        result.feedback.cancelled = true;
        releasePreparedVrrFrame();
        queueRenderDeviceReset();
        return result;
    }

    // Publish the in-flight marker before releasing the presentation mutex so
    // resize/cancellation can retire its GPU writes. Complete rendering here,
    // before the worker's cadence hold, rather than waiting at its deadline.
    m_VrrFramePrepared = true;
    if (!finishVrrPresentReady(true)) {
        const bool frameCancelled = !m_VrrFramePrepared || m_VrrSuspended;
        if (!frameCancelled) {
            m_VrrFallbackReason =
                VrrFallbackReason::AdaptivePresentationUnavailable;
        }
        populateVrrGpuReadyFeedback(result.feedback);
        result.feedback.cancelled = true;
        releasePreparedVrrFrame();
        if (!frameCancelled) {
            queueRenderDeviceReset();
        }
        return result;
    }

    // The readiness wait released the mutex. Revalidate the prepared image
    // and display epoch after reacquiring it, before returning a usable frame.
    if (m_VrrSuspended || checkSupport() != VrrFallbackReason::NoFallback) {
        populateVrrGpuReadyFeedback(result.feedback);
        result.feedback.cancelled = true;
        releasePreparedVrrFrame();
        return result;
    }

    HRESULT deviceReason = m_RenderDevice->GetDeviceRemovedReason();
    if (FAILED(deviceReason)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR preparation detected device loss: %x",
                     deviceReason);
        populateVrrGpuReadyFeedback(result.feedback);
        result.feedback.cancelled = true;
        releasePreparedVrrFrame();
        queueRenderDeviceReset();
        return result;
    }

    result.prepared = true;
    // The exact rendering marker completed successfully. No GPU wait remains
    // at the submission boundary, and the decoder surface is safe to release.
    populateVrrGpuReadyFeedback(result.feedback);
    result.sourceFrameReusable = true;

    // Never hold the presentation mutex while the pacing worker waits for its
    // presentation target. On a shared device it includes FFmpeg's lock, and
    // keeping it here serializes D3D11VA decode behind pacing.
    m_VrrPresentationLocked = false;
    unlockPresentation();
    return result;
}

VrrPresentFeedback D3D11VARenderer::presentAdaptive(
    const VrrPresentRequest& request)
{
    VrrPresentFeedback feedback;

    if (!m_VrrPresentationLocked) {
        lockPresentation();
        m_VrrPresentationLocked = true;
    }

    if (!m_VrrFramePrepared || m_VrrSuspended) {
        return cancelFrame();
    }

    // prepareFrame() already verified this exact marker. Completion cannot
    // move backwards; keep the deadline boundary free of another GPU wait.
    // Cancellation clears that proof and may never admit a replacement marker.
    if (!m_VrrPreparedCompletion.ready(m_VrrGpuReadyFenceValue)) {
        m_VrrFallbackReason =
            VrrFallbackReason::AdaptivePresentationUnavailable;
        populateVrrGpuReadyFeedback(feedback);
        feedback.cancelled = true;
        releasePreparedVrrFrame();
        queueRenderDeviceReset();
        return feedback;
    }

    // A window transition can run during the worker's cadence hold. Revalidate
    // the retained image before touching native state at the target boundary.
    if (m_VrrSuspended || checkSupport() != VrrFallbackReason::NoFallback) {
        populateVrrGpuReadyFeedback(feedback);
        feedback.cancelled = true;
        releasePreparedVrrFrame();
        return feedback;
    }

    const HRESULT deviceReason = m_RenderDevice->GetDeviceRemovedReason();
    if (FAILED(deviceReason)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3D11 VRR presentation detected device loss: %x",
                     deviceReason);
        populateVrrGpuReadyFeedback(feedback);
        feedback.cancelled = true;
        releasePreparedVrrFrame();
        queueRenderDeviceReset();
        return feedback;
    }

    populateVrrGpuReadyFeedback(feedback);

    if (request.collectDiagnostics) {
        // Reuse the observation made after the previous Present. This is the
        // most recent state available before this call, and avoids adding two
        // extra synchronous DXGI queries to every deeply traced frame.
        if (m_VrrPriorPresentCountValid) {
            feedback.presentCountBeforeValid = true;
            feedback.presentCountBefore = m_VrrPriorPresentCount;
        }
        if (m_VrrPriorFrameStatsValid) {
            feedback.frameStatsBeforeValid = true;
            feedback.frameStatsBeforePresentCount =
                m_VrrPriorFrameStatsPresentCount;
            feedback.frameStatsBeforeTimeUs = m_VrrPriorFrameStatsTimeUs;
            feedback.frameStatsBeforePresentRefreshSequence =
                m_VrrPriorFrameStatsPresentRefreshSequence;
            feedback.frameStatsBeforeRefreshSequence =
                m_VrrPriorFrameStatsRefreshSequence;
        }
    }

    feedback.nativeBackendValid = true;
    feedback.nativeBackend = VrrNativePresentationBackend::Dxgi;
    feedback.nativeVrrStateValid = true;
    feedback.nativeTearingSupported = m_VrrTearingSupported;
    feedback.nativeBorderlessFlipModel = m_VrrBorderlessFlipModel;
    feedback.nativeSameGpuOutput = m_VrrSameGpuOutput;
    feedback.nativeRenderAdapterLuidValid =
        m_VrrRenderAdapterLuidValid;
    feedback.nativeRenderAdapterLuid =
        m_VrrRenderAdapterLuid;
    feedback.nativeSwapChainAllowsTearing =
        m_VrrSwapChainAllowsTearing;
    feedback.nativeTearingFeatureQueryResultValid =
        m_VrrTearingFeatureQueryResultValid;
    feedback.nativeTearingFeatureQueryResult =
        m_VrrTearingFeatureQueryResult;
    feedback.nativeTearingFeatureAllowsTearing =
        m_VrrTearingFeatureAllowsTearing;
    feedback.nativeSwapChainDescQueryResultValid =
        m_VrrSwapChainDescQueryResultValid;
    feedback.nativeSwapChainDescQueryResult =
        m_VrrSwapChainDescQueryResult;
    feedback.nativeSwapChainFlags = m_VrrSwapChainFlags;
    feedback.nativeSwapChainSwapEffect =
        m_VrrSwapChainSwapEffect;
    feedback.nativeFullscreenStateQueryResultValid =
        m_VrrFullscreenStateQueryResultValid;
    feedback.nativeFullscreenStateQueryResult =
        m_VrrFullscreenStateQueryResult;
    feedback.nativeFullscreenExclusive =
        m_VrrFullscreenExclusive;
    feedback.nativeWindowFlags = m_VrrWindowFlags;
    feedback.nativePresentReadyAvailable =
        m_VrrPresentReadyAvailable;
    feedback.nativeForegroundWindow =
        m_VrrWindowHandle != nullptr &&
        GetForegroundWindow() == m_VrrWindowHandle;
    feedback.nativeVrrFallbackReason = m_VrrFallbackReason;
    feedback.nativeDesktopMonitorCount = m_VrrDesktopMonitorCount;
    const WindowsVblankVirtualization::Snapshot
        vblankVirtualization =
            WindowsVblankVirtualization::snapshot();
    feedback.nativeVblankVirtualizationProbeComplete =
        vblankVirtualization.probeComplete;
    feedback.nativeVblankVirtualizationCallAvailable =
        vblankVirtualization.callAvailable;
    feedback.nativeVblankVirtualizationResultValid =
        vblankVirtualization.resultValid;
    feedback.nativeVblankVirtualizationResult =
        vblankVirtualization.result;
    feedback.nativeVblankVirtualizationDisabled =
        vblankVirtualization.disabled;
    feedback.nativeDisplayConfigQueryResultValid =
        m_VrrDisplayTiming.queryResultValid;
    feedback.nativeDisplayConfigQueryResult =
        m_VrrDisplayTiming.queryResult;
    feedback.nativeDisplayPathValid = m_VrrDisplayTiming.pathValid;
    feedback.nativeDisplayPathFlags = m_VrrDisplayTiming.pathFlags;
    feedback.nativeDisplayTargetAvailable =
        m_VrrDisplayTiming.targetAvailable;
    feedback.nativeDisplaySourceAdapterLuid =
        m_VrrDisplayTiming.sourceAdapterLuid;
    feedback.nativeDisplaySourceId =
        m_VrrDisplayTiming.sourceId;
    feedback.nativeDisplayTargetAdapterLuid =
        m_VrrDisplayTiming.targetAdapterLuid;
    feedback.nativeDisplayTargetId =
        m_VrrDisplayTiming.targetId;
    feedback.nativeDisplayOutputTechnology =
        m_VrrDisplayTiming.outputTechnology;
    feedback.nativeDisplayRotation =
        m_VrrDisplayTiming.rotation;
    feedback.nativeDisplayScaling =
        m_VrrDisplayTiming.scaling;
    feedback.nativeDisplayPathRefreshNumerator =
        m_VrrDisplayTiming.pathRefreshNumerator;
    feedback.nativeDisplayPathRefreshDenominator =
        m_VrrDisplayTiming.pathRefreshDenominator;
    feedback.nativeDisplaySignalValid =
        m_VrrDisplayTiming.signalValid;
    feedback.nativeDisplaySignalPixelRateHz =
        m_VrrDisplayTiming.signalPixelRateHz;
    feedback.nativeDisplaySignalHSyncNumerator =
        m_VrrDisplayTiming.signalHSyncNumerator;
    feedback.nativeDisplaySignalHSyncDenominator =
        m_VrrDisplayTiming.signalHSyncDenominator;
    feedback.nativeDisplaySignalVSyncNumerator =
        m_VrrDisplayTiming.signalVSyncNumerator;
    feedback.nativeDisplaySignalVSyncDenominator =
        m_VrrDisplayTiming.signalVSyncDenominator;
    feedback.nativeDisplaySignalActiveWidth =
        m_VrrDisplayTiming.signalActiveWidth;
    feedback.nativeDisplaySignalActiveHeight =
        m_VrrDisplayTiming.signalActiveHeight;
    feedback.nativeDisplaySignalTotalWidth =
        m_VrrDisplayTiming.signalTotalWidth;
    feedback.nativeDisplaySignalTotalHeight =
        m_VrrDisplayTiming.signalTotalHeight;
    feedback.nativeDisplaySignalAdditionalInfoRaw =
        m_VrrDisplayTiming.signalAdditionalInfoRaw;
    feedback.nativeDisplaySignalScanLineOrdering =
        m_VrrDisplayTiming.signalScanLineOrdering;
    feedback.nativeRasterSamplingRequested =
        m_VrrRasterSamplingRequested;
    feedback.nativeRasterOpenResultValid =
        m_VrrRasterOpenResultValid;
    feedback.nativeRasterOpenResult = m_VrrRasterOpenResult;
    feedback.nativeRasterSourceValid = m_VrrRasterSourceValid;
    feedback.nativeRasterVidPnSourceId =
        m_VrrRasterVidPnSourceId;
    if (request.collectDiagnostics &&
            m_VrrRasterSamplingRequested &&
            m_VrrRasterSourceValid) {
        feedback.nativeRasterBeforePresent = sampleVrrRaster();
    }
    // Decide after metadata and optional diagnostics, immediately before the
    // native call. A completed present may still be actively scanning out;
    // neither missing statistics nor a CPU spacing floor proves blank.
    // Never spin for blank or disable this guard after observation failures.
    const bool latchedPresentation = D3D11PresentPolicy::latch(
        request.latchedPresentation, m_VrrSyncFlips, request.flipProtectionWindowUs,
        [&]() -> D3D11PresentPolicy::FlipObservation {
            D3D11PresentPolicy::FlipObservation observation;
            feedback.flipProtectionChecked = true;
            feedback.flipProtectionQueryStartUs = LiGetMicroseconds();
            observation.predecessorKnown = m_VrrPriorPresentCountValid;
            DXGI_FRAME_STATISTICS stats = {};
            const HRESULT result = m_SwapChain->GetFrameStatistics(&stats);
            feedback.frameStatsDisjoint = feedback.frameStatsDisjoint ||
                result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
            feedback.flipProtectionQueryResult = static_cast<int64_t>(result);
            observation.statisticsValid = observation.predecessorKnown && result == S_OK &&
                stats.PresentCount <= m_VrrPriorPresentCount;
            observation.pending = observation.predecessorKnown && result == S_OK &&
                stats.PresentCount < m_VrrPriorPresentCount;
            feedback.flipProtectionPending = observation.pending;
            // A handle opened by GDI name is insufficient on cloned or
            // ambiguous outputs. Qualify the current physical path before
            // allowing its raster to authorize adaptive presentation.
            if (observation.statisticsValid && !observation.pending &&
                    feedback.nativeVblankVirtualizationDisabled &&
                    m_VrrSameGpuOutput && m_VrrDisplayTiming.pathValid &&
                    m_VrrDisplayTiming.targetAvailable && m_VrrDisplayTiming.signalValid &&
                    m_VrrRenderAdapterLuidValid &&
                    m_VrrDisplayTiming.sourceAdapterLuid == m_VrrRenderAdapterLuid &&
                    m_VrrDisplayTiming.sourceId == m_VrrRasterVidPnSourceId &&
                    m_VrrRasterMonitor != nullptr &&
                    MonitorFromWindow(m_VrrWindowHandle, MONITOR_DEFAULTTONULL) == m_VrrRasterMonitor) {
                const VrrNativeRasterSample raster = queryVrrRaster();
                feedback.nativeRasterBeforePresent = raster;
                observation.rasterValid = raster.queryResultValid && raster.queryResult == 0;
                observation.inVerticalBlank = raster.inVerticalBlank;
                observation.rasterQueryStartUs = raster.queryStartUs;
                observation.rasterQueryEndUs = raster.queryEndUs;
            }
            feedback.flipProtectionQueryEndUs = LiGetMicroseconds();
            return observation;
        }, [] { return LiGetMicroseconds(); });
    feedback.flipProtectionLatched = !request.latchedPresentation && latchedPresentation;
    // Synchronize unknown or risky observations. A fresh blank is only an
    // adaptive risk filter; it cannot reserve a later asynchronous flip.
    const auto presentParameters = DxgiPresentParameters::adaptive(
        latchedPresentation, DXGI_PRESENT_ALLOW_TEARING);
    feedback.nativePresentParametersValid = true;
    feedback.nativePresentSyncInterval = presentParameters.syncInterval;
    feedback.nativePresentFlags = presentParameters.flags;
    const uint64_t submissionTimeUs = LiGetMicroseconds();
    HRESULT hr = presentPreparedFrame(presentParameters);
    const uint64_t nativePresentEndUs = LiGetMicroseconds();
    if (request.collectDiagnostics &&
            m_VrrRasterSamplingRequested &&
            m_VrrRasterSourceValid) {
        feedback.nativeRasterAfterPresent = sampleVrrRaster();
    }
    feedback.nativePresentResultValid = true;
    feedback.nativePresentResult = static_cast<int64_t>(hr);
    if (request.collectDiagnostics || feedback.nativeRasterBeforePresent.queryResultValid) {
        feedback.nativePresentTimingValid = true;
        feedback.nativePresentStartUs = submissionTimeUs;
        feedback.nativePresentEndUs = nativePresentEndUs;
    }

    if (FAILED(hr)) {
        // Release before notifying the UI about device loss, but not before
        // all successful-Present observations below. Keeping the retained
        // renderer lock across the ID/statistics queries prevents a window or
        // mode reset from replacing the swapchain between Present() and the
        // evidence that is supposed to describe that same call.
        releasePreparedVrrFrame();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() failed on D3D11 VRR path: %x",
                     hr);
        queueRenderDeviceReset();
        feedback.cancelled = true;
        return feedback;
    }
    if (hr != S_OK) {
        // In particular, DXGI_STATUS_OCCLUDED is a successful HRESULT but
        // does not mean that the image reached a monitor.
        releasePreparedVrrFrame();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                     "IDXGISwapChain::Present() returned non-display status on D3D11 VRR path: %x",
                     hr);
        feedback.cancelled = true;
        return feedback;
    }

    feedback.presented = true;
    feedback.submissionTimeValid = true;
    feedback.submissionTimeUs = submissionTimeUs;

    // Observation-only presentation feedback. DXGI frame statistics describe
    // the most recent present that reached the screen, which may lag this
    // submission by several frames. PresentRefreshCount identifies its
    // refresh, but DXGI does not provide that refresh's timestamp.
    // SyncQPCTime is instead paired with SyncRefreshCount and is translated
    // onto the shared pacing clock as a raster-clock anchor. Never treat it as
    // the time of PresentRefreshCount. Failure (for example,
    // FRAME_STATISTICS_DISJOINT after a mode change or a composed swapchain)
    // leaves the sample invalid.
    UINT lastPresentCount = 0;
    if (request.collectDiagnostics) {
        feedback.submissionIdQueryStartUs = LiGetMicroseconds();
    }
    const HRESULT presentCountResult =
        m_SwapChain->GetLastPresentCount(&lastPresentCount);
    if (request.collectDiagnostics) {
        feedback.submissionIdQueryEndUs = LiGetMicroseconds();
    }
    feedback.submissionIdQueryResultValid = true;
    feedback.submissionIdQueryResult =
        static_cast<int64_t>(presentCountResult);
    if (presentCountResult == S_OK) {
        feedback.submissionIdValid = true;
        feedback.submissionId = lastPresentCount;
        m_VrrPriorPresentCountValid = true;
        m_VrrPriorPresentCount = lastPresentCount;
    }
    else {
        m_VrrPriorPresentCountValid = false;
    }

    DXGI_FRAME_STATISTICS frameStats = {};
    uint64_t syncSampleTimeUs = 0;
    uint64_t syncQpcFrequency = 0;
    if (request.collectDiagnostics) {
        feedback.frameStatsQueryStartUs = LiGetMicroseconds();
    }
    const HRESULT frameStatsResult =
        m_SwapChain->GetFrameStatistics(&frameStats);
    if (request.collectDiagnostics) {
        feedback.frameStatsQueryEndUs = LiGetMicroseconds();
    }
    feedback.frameStatsQueryResultValid = true;
    feedback.frameStatsQueryResult =
        static_cast<int64_t>(frameStatsResult);
    feedback.frameStatsDisjoint = feedback.frameStatsDisjoint ||
        frameStatsResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    const bool syncQpcTranslated =
        frameStatsResult == S_OK &&
        translateVrrSyncQpcTime(
            frameStats.SyncQPCTime, syncSampleTimeUs,
            syncQpcFrequency);
    if (frameStatsResult == S_OK &&
            frameStats.SyncQPCTime.QuadPart > 0 &&
            syncQpcFrequency != 0) {
        feedback.latchRawSyncQpcValid = true;
        feedback.latchRawSyncQpcTicks = static_cast<uint64_t>(
            frameStats.SyncQPCTime.QuadPart);
        feedback.latchRawSyncQpcFrequency = syncQpcFrequency;
        const VrrQpcClockCorrelation& correlation =
            vrrQpcClockCorrelation();
        if (correlation.valid &&
                correlation.referenceQpc > 0) {
            feedback.latchQpcCorrelationValid = true;
            feedback.latchQpcCorrelationReferenceTicks =
                static_cast<uint64_t>(correlation.referenceQpc);
            feedback.latchQpcCorrelationReferenceTimeUs =
                correlation.referenceTimeUs;
            feedback.latchQpcCorrelationSpanTicks =
                correlation.bracketSpanTicks;
        }
    }
    if (syncQpcTranslated) {
        feedback.latchSampleValid = true;
        feedback.latchTimeKind = Vrr13::PresentationTimeKind::RefreshReference;
        // Retain the schema-5 field name for compatibility. Its DXGI
        // semantics are the SyncRefreshCount clock sample documented above.
        feedback.latchTimeUs = syncSampleTimeUs;
        feedback.latchSubmissionId = frameStats.PresentCount;
        feedback.latchPresentRefreshSequence =
            frameStats.PresentRefreshCount;
        feedback.latchRefreshSequence = frameStats.SyncRefreshCount;
        m_VrrPriorFrameStatsValid = true;
        m_VrrPriorFrameStatsPresentCount = frameStats.PresentCount;
        m_VrrPriorFrameStatsTimeUs = syncSampleTimeUs;
        m_VrrPriorFrameStatsPresentRefreshSequence =
            frameStats.PresentRefreshCount;
        m_VrrPriorFrameStatsRefreshSequence = frameStats.SyncRefreshCount;
    }
    else {
        m_VrrPriorFrameStatsValid = false;
    }

    // Present(), both raster samples, and the DXGI ID/statistics queries are
    // now one state-consistent observation. Release only after their evidence
    // has been copied into feedback.
    releasePreparedVrrFrame();
    return feedback;
}

VrrPresentFeedback D3D11VARenderer::cancelFrame()
{
    VrrPresentFeedback feedback;
    if (!m_VrrPresentationLocked) {
        lockPresentation();
        m_VrrPresentationLocked = true;
    }
    // A cancelled present still owns queued GPU reads. Drain its marker
    // before the worker may replace its deferred AVFrame or submit another
    // marker on this fence. No native Present is needed for cancellation.
    const bool completionFailed = m_VrrFramePrepared &&
        !finishVrrPresentReady(false);
    populateVrrGpuReadyFeedback(feedback);
    releasePreparedVrrFrame();
    if (completionFailed) {
        m_VrrFallbackReason = VrrFallbackReason::AdaptivePresentationUnavailable;
        queueRenderDeviceReset();
    }
    feedback.cancelled = true;
    return feedback;
}

bool D3D11VARenderer::restoreFixedPresentation(VrrFallbackReason reason)
{
    // This is called synchronously only if the pacing worker could not start,
    // before it has prepared a frame.  ALLOW_TEARING is a swapchain capability,
    // not a requirement for every Present, so the existing swapchain safely
    // supports the legacy fixed path with Present(0, 0).  Do not recreate it.
    cancelFrame();
    // A failed worker startup falls back to the legacy Present-throttled path.
    // The creation flag must survive ResizeBuffers; simply stop CPU admission
    // waits and restore the ordinary queue limit on this still-valid chain.
    if (m_DxgiWaitableActive) {
        if (FAILED(m_SwapChain->SetMaximumFrameLatency(3))) return false;
        m_DxgiWaitableActive = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DXGI waitable pacing disabled for fixed presentation fallback");
    }
    m_VrrSuspended = false;
    m_NonaryVrrRequested = false;
    m_VrrFallbackReason = reason == VrrFallbackReason::NoFallback ?
        VrrFallbackReason::InitializationFailed : reason;
    return true;
}

void D3D11VARenderer::setSuspended(bool suspended)
{
    m_VrrSuspended = suspended;
    if (suspended) {
        // Do not carry a pre-suspension scanout observation into the first
        // diagnostic sample after a window/mode transition.
        m_VrrPriorPresentCountValid = false;
        m_VrrPriorFrameStatsValid = false;
    }
}


void D3D11VARenderer::refreshVrrDisplayTiming()
{
    m_VrrDisplayTiming = {};
    closeVrrRasterSource();
    m_VrrDisplayTiming.queryResultValid = true;
    if (m_VrrWindowHandle == nullptr) {
        m_VrrDisplayTiming.queryResult = ERROR_INVALID_WINDOW_HANDLE;
        return;
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(
        m_VrrWindowHandle, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr ||
            !GetMonitorInfoW(monitor, &monitorInfo)) {
        m_VrrDisplayTiming.queryResult = GetLastError();
        return;
    }

    // Open the raster for the final adaptive-flip veto and optional alignment
    // diagnostics. Protection also requires the unique physical path below;
    // an opened source alone must never qualify a cloned/ambiguous output.
    {
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME openAdapter = {};
        wcsncpy_s(openAdapter.DeviceName, monitorInfo.szDevice, _TRUNCATE);
        const NTSTATUS openResult =
            D3DKMTOpenAdapterFromGdiDisplayName(&openAdapter);
        m_VrrRasterOpenResultValid = true;
        m_VrrRasterOpenResult = static_cast<int64_t>(openResult);
        if (openResult == 0) {
            m_VrrRasterSourceValid = true;
            m_VrrRasterMonitor = monitor;
            m_VrrRasterAdapter = openAdapter.hAdapter;
            m_VrrRasterVidPnSourceId = openAdapter.VidPnSourceId;
        }
    }

    // Windows 11 can expose a virtual desktop refresh independently from the
    // physical signal refresh. Prefer that view, then fall back for older
    // systems that reject the newer query flag.
    constexpr UINT32 queryFlagSets[] = {
        QDC_ONLY_ACTIVE_PATHS |
            QDC_VIRTUAL_MODE_AWARE |
            QDC_VIRTUAL_REFRESH_RATE_AWARE,
        QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
        QDC_ONLY_ACTIVE_PATHS,
    };
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG queryResult = ERROR_INVALID_PARAMETER;
    for (UINT32 flags : queryFlagSets) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            UINT32 pathCount = 0;
            UINT32 modeCount = 0;
            queryResult = GetDisplayConfigBufferSizes(
                flags, &pathCount, &modeCount);
            if (queryResult != ERROR_SUCCESS) {
                break;
            }
            paths.assign(pathCount, DISPLAYCONFIG_PATH_INFO {});
            modes.assign(modeCount, DISPLAYCONFIG_MODE_INFO {});
            queryResult = QueryDisplayConfig(
                flags, &pathCount,
                paths.empty() ? nullptr : paths.data(),
                &modeCount,
                modes.empty() ? nullptr : modes.data(),
                nullptr);
            if (queryResult == ERROR_SUCCESS) {
                paths.resize(pathCount);
                modes.resize(modeCount);
                break;
            }
            if (queryResult != ERROR_INSUFFICIENT_BUFFER) {
                break;
            }
        }
        if (queryResult == ERROR_SUCCESS ||
                queryResult != ERROR_INVALID_PARAMETER) {
            break;
        }
    }
    m_VrrDisplayTiming.queryResult =
        static_cast<uint32_t>(queryResult);
    if (queryResult != ERROR_SUCCESS) {
        return;
    }

    VrrDisplayTimingSnapshot matchedTiming;
    uint32_t matchedPaths = 0;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) !=
                ERROR_SUCCESS ||
                _wcsicmp(sourceName.viewGdiDeviceName,
                         monitorInfo.szDevice) != 0) {
            continue;
        }

        ++matchedPaths;
        matchedTiming.queryResultValid = true;
        matchedTiming.queryResult = ERROR_SUCCESS;
        matchedTiming.pathValid = true;
        matchedTiming.pathFlags = path.flags;
        matchedTiming.targetAvailable =
            path.targetInfo.targetAvailable != FALSE;
        matchedTiming.sourceAdapterLuid =
            packLuid(path.sourceInfo.adapterId);
        matchedTiming.sourceId = path.sourceInfo.id;
        matchedTiming.targetAdapterLuid =
            packLuid(path.targetInfo.adapterId);
        matchedTiming.targetId = path.targetInfo.id;
        matchedTiming.outputTechnology =
            static_cast<uint32_t>(
                path.targetInfo.outputTechnology);
        matchedTiming.rotation =
            static_cast<uint32_t>(path.targetInfo.rotation);
        matchedTiming.scaling =
            static_cast<uint32_t>(path.targetInfo.scaling);
        matchedTiming.pathRefreshNumerator =
            path.targetInfo.refreshRate.Numerator;
        matchedTiming.pathRefreshDenominator =
            path.targetInfo.refreshRate.Denominator;

        const bool virtualMode =
            (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0;
        const UINT32 targetModeIndex = virtualMode ?
            path.targetInfo.targetModeInfoIdx :
            path.targetInfo.modeInfoIdx;
        const UINT32 invalidTargetModeIndex = virtualMode ?
            DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID :
            DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        if (targetModeIndex == invalidTargetModeIndex ||
                targetModeIndex >= modes.size()) {
            continue;
        }
        const DISPLAYCONFIG_MODE_INFO& mode = modes[targetModeIndex];
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
                mode.id != path.targetInfo.id ||
                mode.adapterId.HighPart !=
                    path.targetInfo.adapterId.HighPart ||
                mode.adapterId.LowPart !=
                    path.targetInfo.adapterId.LowPart) {
            continue;
        }

        const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal =
            mode.targetMode.targetVideoSignalInfo;
        matchedTiming.signalValid = true;
        matchedTiming.signalPixelRateHz = signal.pixelRate;
        matchedTiming.signalHSyncNumerator =
            signal.hSyncFreq.Numerator;
        matchedTiming.signalHSyncDenominator =
            signal.hSyncFreq.Denominator;
        matchedTiming.signalVSyncNumerator =
            signal.vSyncFreq.Numerator;
        matchedTiming.signalVSyncDenominator =
            signal.vSyncFreq.Denominator;
        matchedTiming.signalActiveWidth = signal.activeSize.cx;
        matchedTiming.signalActiveHeight = signal.activeSize.cy;
        matchedTiming.signalTotalWidth = signal.totalSize.cx;
        matchedTiming.signalTotalHeight = signal.totalSize.cy;
        // Preserve the complete union word. Bits 0..15 are videoStandard,
        // bits 16..21 are the Miracast VSync divider, and the remainder is
        // reserved. Keeping the raw value avoids hiding future driver data.
        matchedTiming.signalAdditionalInfoRaw = signal.videoStandard;
        matchedTiming.signalScanLineOrdering =
            static_cast<uint32_t>(signal.scanLineOrdering);
    }

    // A cloned source can map the same GDI name to more than one target.
    // Refuse to guess which physical signal owns the window.
    if (matchedPaths == 1) {
        m_VrrDisplayTiming = matchedTiming;
    }
}

void D3D11VARenderer::closeVrrRasterSource()
{
    if (m_VrrRasterSourceValid) {
        D3DKMT_CLOSEADAPTER closeAdapter = {};
        closeAdapter.hAdapter = m_VrrRasterAdapter;
        D3DKMTCloseAdapter(&closeAdapter);
    }
    m_VrrRasterOpenResultValid = false;
    m_VrrRasterOpenResult = 0;
    m_VrrRasterSourceValid = false;
    m_VrrRasterMonitor = nullptr;
    m_VrrRasterAdapter = 0;
    m_VrrRasterVidPnSourceId = 0;
}

VrrNativeRasterSample D3D11VARenderer::sampleVrrRaster() const
{
    if (!m_VrrRasterSamplingRequested) {
        return {};
    }
    return queryVrrRaster();
}

VrrNativeRasterSample D3D11VARenderer::queryVrrRaster() const
{
    VrrNativeRasterSample sample;
    if (!m_VrrRasterSourceValid) {
        return sample;
    }

    D3DKMT_GETSCANLINE query = {};
    query.hAdapter = m_VrrRasterAdapter;
    query.VidPnSourceId = m_VrrRasterVidPnSourceId;
    sample.queryStartUs = LiGetMicroseconds();
    const NTSTATUS queryResult = D3DKMTGetScanLine(&query);
    sample.queryEndUs = LiGetMicroseconds();
    sample.queryResultValid = true;
    sample.queryResult = static_cast<int64_t>(queryResult);
    if (queryResult == 0) {
        sample.inVerticalBlank = query.InVerticalBlank != FALSE;
        sample.scanLine = query.ScanLine;
    }
    return sample;
}

