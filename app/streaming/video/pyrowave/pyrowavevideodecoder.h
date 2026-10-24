#pragma once

#include "pyrowavecolor.h"
#include "pyrowavepacing.h"
#include "streaming/bandwidth.h"
#include "streaming/video/decoder.h"
#include "streaming/video/overlaymanager.h"

#include <QCoreApplication>
#include <QString>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

// User event that shows the one-time "PyroWave wasn't used" notice once the
// stream window exists. data1 carries the negotiated video format. The value
// is kept well away from the SDL_CODE_* range used in session.cpp.
#define SDL_CODE_PYROWAVE_FALLBACK_NOTICE 200

// User event that ends the stream after PyroWave failed three times in a row.
// data1 carries the PyrowaveStage, data2 a heap-allocated QString with the
// translated reason, which the handler deletes. See fatalErrorText().
#define SDL_CODE_PYROWAVE_FATAL 201

// Whether a host offered PyroWave (x-ss-pyrowave.* in its RTSP DESCRIBE reply)
// on its most recent connection. Stored per host by PyrowaveHostStore.
enum class PyrowaveHostOffer
{
    Unknown = 0,
    Offered = 1,
    NotOffered = 2,
};

struct PyrowavePreflightRequest
{
    SDL_Window* window;
    int width;
    int height;
    int frameRate;
    // 0 = SDR, 1 = HDR10 (PQ), 2 = HLG, as in STREAM_CONFIGURATION.hdrMode.
    int hdrMode;
};

struct PyrowavePreflightResult
{
    bool ok;
    // Translated reason shown instead of connecting when ok is false.
    QString reason;
};

// What the session negotiated that the decoder must follow. Session fills
// it in chooseDecoder() from its stream configuration.
struct PyrowaveSessionContract
{
    // COLOR_RANGE_LIMITED or COLOR_RANGE_FULL (StreamConfig.colorRange). The
    // host encodes with it, and every frame's native metadata must match.
    int colorRange = COLOR_RANGE_LIMITED;
    // 0 = SDR, 1 = HDR10, 2 = HLG (StreamConfig.hdrMode)
    int hdrMode = 0;
    // CAPABILITY_PULL_RENDERER was advertised: frames are pulled with
    // LiWaitForNextVideoFrame() on our own thread instead of being pushed to
    // submitDecodeUnit().
    bool pullRenderer = false;
};

#ifdef HAVE_LIBPLACEBO_VULKAN
class PyrowaveVulkanInstance;
class PyrowaveVulkanRenderer;
struct PyrowaveFailure;
#endif

// Decoder for the experimental PyroWave codec (see the Moonlight V+ client
// contract, docs/pyrowave-client-contract.md in qiin2333/moonlight-vplus).
//
// One Vulkan device is shared by pyrowave_create_device() and libplacebo
// (PyrowaveVulkanRenderer). Each frame is decoded into Y/Cb/Cr planes and
// presented through a libplacebo swapchain, with the stats overlay drawn on
// top, on the thread that receives the frame. CAPABILITY_DIRECT_SUBMIT stays
// off because a frame can wait for the GPU and the window.
//
// Frame results follow the contract: an incomplete frame keeps the decoder
// and the last image; any failure rebuilds the renderer, and three in a row
// end the connection with an error naming the stage. The codec never changes
// mid-stream.
//
// HDR10 and HLG sessions decode into 16-bit planes and present through an
// HDR swapchain with the host's static metadata (SS_HDR_METADATA). Whether a
// session is SDR or HDR is fixed before it starts; preflight() refuses HDR on
// a GPU or display that can't show it rather than streaming SDR instead.
//
// With V-Sync and frame pacing on, a stream faster than the display presents
// an even share of its frames (PyrowavePacing::FramePacer). The swapchain
// starts double-buffered and switches to triple buffering after delayed
// presents.
//
// Not yet: dynamic HDR metadata, the SDR CPU-staging fallback.
class PyrowaveVideoDecoder : public IVideoDecoder, public Overlay::IOverlayRenderer
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveVideoDecoder)

public:
#ifdef HAVE_LIBPLACEBO_VULKAN
    static constexpr bool k_DecoderImplemented = true;
#else
    // The decoder needs libplacebo's Vulkan renderer.
    static constexpr bool k_DecoderImplemented = false;
#endif

    // Contract limit for the GPU path (both dimensions even, at most 8192).
    static constexpr int k_MaxDimension = 8192;

    // Consecutive failures before the connection ends (client contract,
    // section 6).
    static constexpr int k_MaxConsecutiveFailures = 3;

    PyrowaveVideoDecoder(bool testOnly, const PyrowaveSessionContract& contract);
    virtual ~PyrowaveVideoDecoder() override;

    // Creates and releases a candidate device, decoder and swapchain at the
    // requested size on the request's window. Runs before ANNOUNCE, so a GPU
    // or window that can't do it fails here with a specific reason.
    static PyrowavePreflightResult preflight(const PyrowavePreflightRequest& request);

    // Adds PyroWave to the formats and capabilities that Session gives
    // moonlight-common-c. Both return their input unchanged without the
    // decoder, so such a build can never negotiate PyroWave.
    static int advertisedVideoFormats(int supportedVideoFormats);
    static int advertisedCapabilities(int fallbackDecoderCapabilities);

    // What this connection tells us about the host's PyroWave support.
    static PyrowaveHostOffer hostOfferFromConnection(int negotiatedVideoFormat);

    // Translated texts used by Session.
    static QString launchErrorText(const QString& reason);
    static QString fallbackNotice(int negotiatedVideoFormat);
    static QString videoFormatDisplayName(int videoFormat);
    static QString fatalErrorText(int stage, const QString& reason);

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override { return true; }
    virtual bool isAlwaysFullScreen() override { return false; }
    // True once a renderer was built for this HDR session, which checked the
    // GPU, the driver and the window for HDR output. False for SDR sessions,
    // whose device was never checked for it.
    virtual bool isHdrSupported() override { return m_HdrVerified; }
    virtual int getDecoderCapabilities() override { return CAPABILITY_PYROWAVE; }
    // SDR PyroWave is BT.709, HDR10 and HLG are BT.2020 (client contract,
    // section 3).
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override { return m_Contract.colorRange; }
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override {}
    // Takes the host's static HDR metadata (LiGetHdrMetadata()) for the
    // frames and the swapchain. Never changes the session between SDR and
    // HDR: the stream's signal was fixed when it was negotiated.
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override;

private:
    static int decoderThreadProc(void* context);

    int processFrame(PDECODE_UNIT du);
    void accountReceivedFrame(PDECODE_UNIT du);
    void publishStatsWindow(const VIDEO_STATS& recent, const VIDEO_STATS& completed);
    void stringifyVideoStats(const VIDEO_STATS& stats, char* output, int length) const;

    bool m_TestOnly;
    PyrowaveSessionContract m_Contract;
    int m_Width = 0;
    int m_Height = 0;
    int m_StreamFps = 0;
    SDL_Window* m_Window = nullptr;

    // Refresh rate of the window's display, 0 if unknown. Written on the
    // main thread when the window moves, read by the decode thread.
    std::atomic<int> m_DisplayFps{ 0 };
    // Decode thread
    bool m_EnableFramePacing = false;
    int m_PacerDisplayFps = 0;
    PyrowavePacing::FramePacer m_FramePacer;

    // Wire-level video bitrate (with FEC), read once per stats window
    BandwidthMeter m_BandwidthMeter;
    double m_BandwidthMbps = std::nan("");
    // Describe the current renderer for the stats model. Decode thread.
    QString m_DecoderName;
    QString m_RendererName;

    // Video stats windows, as FFmpegVideoDecoder keeps them
    VIDEO_STATS m_ActiveWndVideoStats = {};
    VIDEO_STATS m_LastWndVideoStats = {};
    VIDEO_STATS m_GlobalVideoStats = {};
    int m_LastFrameNumber = 0;

    std::vector<uint8_t> m_FrameBuffer;

    SDL_Thread* m_DecoderThread = nullptr;
    SDL_atomic_t m_DecoderThreadShouldQuit = {};

    // Written by setHdrMode() on the control thread, copied by the decode
    // thread for every frame. Outlives renderer rebuilds, so a new renderer
    // starts with the last accepted metadata.
    std::mutex m_HdrLock;
    PyrowaveColor::HdrSnapshot m_HdrSnapshot;
    // Set on the decode thread, read on the main thread.
    std::atomic<bool> m_HdrVerified{ false };

#ifdef HAVE_LIBPLACEBO_VULKAN
    bool createRenderer(PyrowaveFailure& failure);
    void noteFailure(const PyrowaveFailure& failure);

    std::unique_ptr<PyrowaveVulkanInstance> m_Instance;
    // Serializes the decode thread, overlay updates and teardown.
    std::mutex m_RendererLock;
    std::unique_ptr<PyrowaveVulkanRenderer> m_Renderer;
    bool m_EnableVsync = false;
    bool m_IgnoreAspectRatio = false;
    int m_ConsecutiveFailures = 0;
    bool m_Fatal = false;
#endif
};
