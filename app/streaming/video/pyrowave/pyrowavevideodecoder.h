#pragma once

#include "streaming/video/decoder.h"

#include <QCoreApplication>
#include <QString>

// User event that shows the one-time "PyroWave wasn't used" notice once the
// stream window exists. data1 carries the negotiated video format. The value
// is kept well away from the SDL_CODE_* range used in session.cpp.
#define SDL_CODE_PYROWAVE_FALLBACK_NOTICE 200

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

// Decoder for the experimental PyroWave codec (see the Moonlight V+ client
// contract, docs/pyrowave-client-contract.md in qiin2333/moonlight-vplus).
//
// PW-1 status: this is a stub. The settings, negotiation plumbing and runtime
// loader exist, but there is no Vulkan decoder yet, so preflight() always
// fails and Session never advertises VIDEO_FORMAT_PYROWAVE.
//
// PW-2 plan: one Vulkan device shared by pyrowave_create_device() and
// libplacebo (pl_vulkan_import), decoded planes wrapped with pl_vulkan_wrap
// and presented like the plvk renderer. To land it:
//   1. implement the IVideoDecoder methods below and set k_DecoderImplemented;
//   2. make preflight() create (and release) a candidate device and decoder
//      at the requested size, HDR mode and window, as the contract requires;
//   3. keep CAPABILITY_DIRECT_SUBMIT off (submits may wait on the GPU) and
//      accept whatever delivery model the H.264 fallback decoder chose, since
//      Session reports one set of video callback capabilities per stream.
class PyrowaveVideoDecoder : public IVideoDecoder
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveVideoDecoder)

public:
    // Flip to true only together with a working decoder. Every path that
    // could advertise PyroWave checks this first.
    static constexpr bool k_DecoderImplemented = false;

    // Contract limit for the GPU path (both dimensions even, at most 8192).
    static constexpr int k_MaxDimension = 8192;

    explicit PyrowaveVideoDecoder(bool testOnly) : m_TestOnly(testOnly) {}
    virtual ~PyrowaveVideoDecoder() override = default;

    // Checks everything that must hold before advertising PyroWave for this
    // stream: the runtime, the decoder and (from PW-2) the GPU and window.
    static PyrowavePreflightResult preflight(const PyrowavePreflightRequest& request);

    // Adds PyroWave to the formats and capabilities that Session gives
    // moonlight-common-c. Both return their input unchanged until the
    // decoder exists, so a stub build can never negotiate PyroWave.
    static int advertisedVideoFormats(int supportedVideoFormats);
    static int advertisedCapabilities(int fallbackDecoderCapabilities);

    // What this connection tells us about the host's PyroWave support.
    static PyrowaveHostOffer hostOfferFromConnection(int negotiatedVideoFormat);

    // Translated texts used by Session.
    static QString launchErrorText(const QString& reason);
    static QString fallbackNotice(int negotiatedVideoFormat);
    static QString videoFormatDisplayName(int videoFormat);

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override { return true; }
    virtual bool isAlwaysFullScreen() override { return false; }
    virtual bool isHdrSupported() override { return false; }
    virtual int getDecoderCapabilities() override { return CAPABILITY_PYROWAVE; }
    // SDR PyroWave is BT.709 (client contract, section 3).
    virtual int getDecoderColorspace() override { return COLORSPACE_REC_709; }
    virtual int getDecoderColorRange() override { return COLOR_RANGE_LIMITED; }
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override {}
    virtual void setHdrMode(bool) override {}
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override { return false; }

private:
    bool m_TestOnly;
};
