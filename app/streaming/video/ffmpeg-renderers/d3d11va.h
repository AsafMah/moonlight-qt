#pragma once

#include "renderer.h"
#include "dxgipresent.h"
#include "dxgiwaitable.h"
#include "d3d11presentpolicy.h"
#include "ivrrframepresenter.h"

#include <d3d11_4.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <CGuid.h>
#include "streaming/video/videoenhancement.h"
#include "amfsession.h"

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

// IVrrFramePresenter is the Nonary VRR pacer's renderer contract (ported from
// Nonary/moonlight-qt@c822745). It is only active when the session selects
// the Nonary VRR implementation; the fork's VRR mode and the fixed/unpaced
// paths keep using renderFrame().
class D3D11VARenderer : public IFFmpegRenderer, public IVrrFramePresenter
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual bool readyToPresentAfterWait() override;
    virtual bool takeRenderTiming(RenderTiming* timing) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderCapabilities() override;
    virtual bool needsTestFrame();
    virtual void setHdrMode(bool enabled) override;
    virtual InitFailureReason getInitFailureReason() override;

    // IVrrFramePresenter (Nonary VRR pacer, DXGI path)
    virtual IVrrFramePresenter* getVrrFramePresenter() override;
    virtual bool canLatchAdaptivePresent() const override { return true; }
    void interruptFrameWait(bool stopping) override { m_DxgiAdmission.interrupt(stopping); }
    virtual VrrFallbackReason checkSupport() const override;
    virtual uint64_t captureDecodeBoundary() override;
    uint64_t waitForDecode(AVFrame* frame, uint64_t decodeBoundary) override;
    VrrDecodeReadiness observeDecodeReadiness(AVFrame* frame, uint64_t decodeBoundary) override;
    virtual VrrPrepareResult prepareFrame(AVFrame* frame, uint64_t decodeBoundary) override;
    virtual VrrPresentFeedback presentAdaptive(const VrrPresentRequest& request) override;
    virtual VrrPresentFeedback cancelFrame() override;
    virtual void setSuspended(bool suspended) override;
    virtual bool restoreFixedPresentation(VrrFallbackReason reason) override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
        _COUNT
    };

private:
    static void lockContext(void* lock_ctx);
    static void unlockContext(void* lock_ctx);

    bool setupRenderingResources();
    std::vector<DXGI_FORMAT> getVideoTextureSRVFormats();
    bool setupFrameRenderingResources(AVHWFramesContext* framesContext);
    bool setupSwapchainDependentResources();
    bool setupVideoTexture(AVHWFramesContext* framesContext); // for !m_BindDecoderOutputTextures
    bool setupTexturePoolViews(AVHWFramesContext* framesContext); // for m_BindDecoderOutputTextures
    void renderOverlay(Overlay::OverlayType type);
    bool createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, Microsoft::WRL::ComPtr<ID3D11Buffer>& newVertexBuffer);
    void bindColorConversion(bool frameChanged, AVFrame* frame);
    void bindVideoVertexBuffer(bool frameChanged, AVFrame* frame);
    // Returns false only if the frame must not be presented (Nonary VRR fence failure)
    bool renderVideo(AVFrame* frame, uint64_t decodeBoundary = 0);
    // Everything renderFrame() does before Present(); returns false if the
    // frame must not be presented. Caller holds the render locks.
    bool prepareFrameForPresent(AVFrame* frame, uint64_t decodeBoundary = 0);
    bool createVideoProcessor(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr);
    bool initializeVideoProcessor();
    bool enableAMDVideoSuperResolution(bool activate = true, bool logInfo = true, ID3D11Device* device = nullptr);
    bool enableIntelVideoSuperResolution(bool activate = true, bool logInfo = true);
    bool enableNvidiaVideoSuperResolution(bool activate = true, bool logInfo = true);
    bool enableAMDHDR(bool activate = true, bool logInfo = true);
    bool enableIntelHDR(bool activate = true, bool logInfo = true);
    bool enableNvidiaHDR(bool activate = true, bool logInfo = true);
    bool prepareEnhancedOutput(AVFrame* frame);
    bool rebuildVideoProcessorFallback(AVFrame* frame);
    bool setupAmfTexture();
    bool setupEnhancedTexture();
    int getAdapterIndexByEnhancementCapabilities();
    bool checkDecoderSupport(IDXGIAdapter* adapter);
    bool createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound = nullptr);
    bool setupSharedDevice(IDXGIAdapter1* adapter);
    bool createSharedFencePair(UINT64 initialValue,
                               ID3D11Device5* dev1, ID3D11Device5* dev2,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev1Fence,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev2Fence);

    int m_DecoderSelectionPass;
    int m_DevicesWithFL11Support;
    int m_DevicesWithCodecSupport;

    enum class SupportedFenceType {
        None,
        NonMonitored,
        Monitored,
    };

    bool m_DebugLayer;
    Microsoft::WRL::ComPtr<IDXGIFactory5> m_Factory;
    int m_AdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    // Signaled when the swapchain can take another frame without blocking
    HANDLE m_FrameLatencyWaitable = nullptr;
    // The last waitToRender() saw the waitable object signaled
    bool m_ReadyToPresent = false;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    bool m_BindDecoderOutputTextures;
    bool m_UseFenceHack;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_VideoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext2> m_VideoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_VideoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_VideoProcessorEnumerator;
    D3D11_VIDEO_PROCESSOR_CAPS m_VideoProcessorCapabilities;
    D3D11_VIDEO_PROCESSOR_STREAM m_StreamData;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_OutputView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_InputView;
    Microsoft::WRL::ComPtr<ID3D11Resource> m_BackBufferResource;
    VideoEnhancement* m_VideoEnhancement;
    bool m_AutoStreamSuperResolution = false;
    bool m_VideoProcessorOutputRGBA = false;
    bool m_RenderResetPending = false;
    bool m_HasRenderTiming = false;
    RenderTiming m_RenderTiming;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    int m_OutputIndex;
    AVColorTransferCharacteristic m_LastColorTrc;
    int m_LastColorSpace;
    bool m_LastFullRange;
    int m_LastEnhColorTrc;

    struct {
        int width;
        int height;
        int left;
        int top;
    } m_OutputTexture;
    D3D11_BOX m_SrcBox;

    bool m_AllowTearing;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_AmfTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_EnhancedTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;

    // AMD (AMF)
    AmfSession m_Amf;
    bool m_AmfUpScalerSharpness = false;

    // --- Nonary VRR presenter (d3d11vrr.cpp, ported from Nonary/moonlight-qt@c822745) ---
    void lockPresentation();
    void unlockPresentation();
    bool initializeVrrPresentReadyFence();
    bool beginVrrPresentReady();
    bool finishVrrPresentReady(bool releasePresentationWhileWaiting);
    HRESULT presentPreparedFrame(const DxgiPresentParameters& parameters);
    void initializeVrrPresentationState(SDL_Window* window, DXGI_SWAP_CHAIN_DESC1* swapChainDesc);
    void refreshVrrDisplayState();
    void refreshVrrDisplayTiming();
    void closeVrrRasterSource();
    VrrNativeRasterSample sampleVrrRaster() const;
    VrrNativeRasterSample queryVrrRaster() const;
    void populateVrrGpuReadyFeedback(VrrPresentFeedback& feedback) const;
    VrrFallbackReason evaluateVrrEligibility(bool prioritizeOutputCompatibility);
    bool retirePreparedVrrFrameForMutation();
    void releasePreparedVrrFrame();
    void queueRenderDeviceReset();

    struct VrrDisplayTimingSnapshot
    {
        bool queryResultValid = false;
        uint32_t queryResult = 0;
        bool pathValid = false;
        uint32_t pathFlags = 0;
        bool targetAvailable = false;
        uint64_t sourceAdapterLuid = 0;
        uint32_t sourceId = 0;
        uint64_t targetAdapterLuid = 0;
        uint32_t targetId = 0;
        uint32_t outputTechnology = 0;
        uint32_t rotation = 0;
        uint32_t scaling = 0;
        uint32_t pathRefreshNumerator = 0;
        uint32_t pathRefreshDenominator = 0;
        bool signalValid = false;
        uint64_t signalPixelRateHz = 0;
        uint32_t signalHSyncNumerator = 0;
        uint32_t signalHSyncDenominator = 0;
        uint32_t signalVSyncNumerator = 0;
        uint32_t signalVSyncDenominator = 0;
        uint32_t signalActiveWidth = 0;
        uint32_t signalActiveHeight = 0;
        uint32_t signalTotalWidth = 0;
        uint32_t signalTotalHeight = 0;
        uint32_t signalAdditionalInfoRaw = 0;
        uint32_t signalScanLineOrdering = 0;
    };

    // enableVrr && vrrImplementation == Nonary; cleared by restoreFixedPresentation()
    bool m_NonaryVrrRequested = false;
    int m_RenderAdapterIndex = -1;
    // Lock order: m_PresentationLock, then m_ContextLock
    SDL_mutex* m_PresentationLock = nullptr;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits>
        m_DxgiFrameLatencyHandle;
    DxgiWaitable::Admission m_DxgiAdmission;
    bool m_DxgiWaitableActive = false;
    uint64_t m_DxgiWaitWarningUs = 0;
    bool m_VrrRenderAdapterLuidValid = false;
    uint64_t m_VrrRenderAdapterLuid = 0;
    bool m_VrrTearingSupported = false;
    bool m_VrrBorderlessFlipModel = false;
    bool m_VrrSameGpuOutput = false;
    bool m_VrrSwapChainAllowsTearing = false;
    bool m_VrrTearingFeatureQueryResultValid = false;
    int64_t m_VrrTearingFeatureQueryResult = 0;
    bool m_VrrTearingFeatureAllowsTearing = false;
    bool m_VrrSwapChainDescQueryResultValid = false;
    int64_t m_VrrSwapChainDescQueryResult = 0;
    uint32_t m_VrrSwapChainFlags = 0;
    uint32_t m_VrrSwapChainSwapEffect = 0;
    bool m_VrrFullscreenStateQueryResultValid = false;
    int64_t m_VrrFullscreenStateQueryResult = 0;
    bool m_VrrFullscreenExclusive = false;
    uint32_t m_VrrWindowFlags = 0;
    UINT m_VrrDesktopMonitorCount = 0;
    HWND m_VrrWindowHandle = nullptr;
    VrrDisplayTimingSnapshot m_VrrDisplayTiming;
    bool m_VrrRasterSamplingRequested = false;
    // MOONLIGHT_VRR_SYNC_FLIPS=1 synchronizes every VRR flip
    bool m_VrrSyncFlips = false;
    bool m_VrrRasterOpenResultValid = false;
    int64_t m_VrrRasterOpenResult = 0;
    bool m_VrrRasterSourceValid = false;
    HMONITOR m_VrrRasterMonitor = nullptr;
    D3DKMT_HANDLE m_VrrRasterAdapter = 0;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_VrrRasterVidPnSourceId = 0;
    bool m_VrrSuspended = false;
    VrrFallbackReason m_VrrFallbackReason = VrrFallbackReason::InitializationFailed;
    bool m_VrrFramePrepared = false;
    D3D11PresentPolicy::PreparedCompletion m_VrrPreparedCompletion;
    uint64_t m_VrrPreparedDecodeBoundary = 0;
    bool m_VrrPresentationLocked = false;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_VrrPresentReadyFence;
    UINT64 m_VrrPresentReadyFenceValue = 0;
    HANDLE m_VrrPresentReadyFenceEvent = nullptr;
    HANDLE m_VrrDecodeReadyEvent = nullptr;
    bool m_VrrPresentReadyAvailable = false;
    bool m_VrrGpuReadyAttempted = false;
    bool m_VrrGpuReadySignalResultValid = false;
    int64_t m_VrrGpuReadySignalResult = 0;
    bool m_VrrGpuReadySetEventResultValid = false;
    int64_t m_VrrGpuReadySetEventResult = 0;
    bool m_VrrGpuReadyWaitResultValid = false;
    uint32_t m_VrrGpuReadyWaitResult = 0;
    bool m_VrrGpuReadyTimingValid = false;
    uint64_t m_VrrGpuReadySignalStartUs = 0;
    uint64_t m_VrrGpuReadySignalEndUs = 0;
    uint64_t m_VrrGpuReadyFlushStartUs = 0;
    uint64_t m_VrrGpuReadyFlushEndUs = 0;
    uint64_t m_VrrGpuReadySetEventStartUs = 0;
    uint64_t m_VrrGpuReadySetEventEndUs = 0;
    uint64_t m_VrrGpuReadyPollStartUs = 0;
    uint64_t m_VrrGpuReadyPollEndUs = 0;
    uint64_t m_VrrGpuReadyFenceValue = 0;
    uint64_t m_VrrGpuReadyPollCompletedValue = 0;
    bool m_VrrGpuReadyCompletedBeforeWait = false;
    uint64_t m_VrrGpuReadyWaitStartUs = 0;
    uint64_t m_VrrGpuReadyTimeUs = 0;
    bool m_VrrPriorPresentCountValid = false;
    uint64_t m_VrrPriorPresentCount = 0;
    bool m_VrrPriorFrameStatsValid = false;
    uint64_t m_VrrPriorFrameStatsPresentCount = 0;
    uint64_t m_VrrPriorFrameStatsTimeUs = 0;
    uint64_t m_VrrPriorFrameStatsPresentRefreshSequence = 0;
    uint64_t m_VrrPriorFrameStatsRefreshSequence = 0;
};
