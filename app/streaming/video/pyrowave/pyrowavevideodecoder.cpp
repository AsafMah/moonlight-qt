#include "pyrowavevideodecoder.h"
#include "pyrowaveruntime.h"

#include <QSize>

PyrowavePreflightResult PyrowaveVideoDecoder::preflight(const PyrowavePreflightRequest& request)
{
    PyrowavePreflightResult result;
    result.ok = false;

    const PyrowaveRuntime& runtime = PyrowaveRuntime::get();
    if (!runtime.isAvailable()) {
        result.reason = runtime.userMessage();
        return result;
    }

    const bool evenSize = (request.width % 2) == 0 && (request.height % 2) == 0;
    const bool inRange = request.width > 0 && request.height > 0 &&
                         request.width <= k_MaxDimension && request.height <= k_MaxDimension;
    if (!evenSize || !inRange) {
        result.reason = tr("PyroWave needs an even resolution up to %1x%1 (selected: %2x%3).")
                            .arg(k_MaxDimension)
                            .arg(request.width)
                            .arg(request.height);
        return result;
    }

    if (!k_DecoderImplemented) {
        result.reason = tr("The PyroWave decoder isn't implemented in this build yet.");
        return result;
    }

    // PW-2: create a candidate Vulkan device and decoder with the request's
    // size, HDR mode and window, then release them. Fail here, before
    // ANNOUNCE, rather than after the host has started encoding. HDR10 must
    // also confirm VK_EXT_hdr_metadata (client contract, section 5).
    result.ok = true;
    return result;
}

int PyrowaveVideoDecoder::advertisedVideoFormats(int supportedVideoFormats)
{
    if (!k_DecoderImplemented) {
        return supportedVideoFormats;
    }

    // moonlight-common-c tries PyroWave first and falls back to the legacy
    // formats in supportedVideoFormats (H.264 for PyroWave sessions).
    return supportedVideoFormats | VIDEO_FORMAT_PYROWAVE;
}

int PyrowaveVideoDecoder::advertisedCapabilities(int fallbackDecoderCapabilities)
{
    if (!k_DecoderImplemented) {
        return fallbackDecoderCapabilities;
    }

    // PyroWave submits can wait on the GPU and the window, so frames must
    // not be decoded on the receive thread (client decoder design, section 3).
    return (fallbackDecoderCapabilities & ~CAPABILITY_DIRECT_SUBMIT) | CAPABILITY_PYROWAVE;
}

PyrowaveHostOffer PyrowaveVideoDecoder::hostOfferFromConnection(int negotiatedVideoFormat)
{
    if (negotiatedVideoFormat == VIDEO_FORMAT_PYROWAVE) {
        return PyrowaveHostOffer::Offered;
    }

    // moonlight-common-c parses x-ss-pyrowave.* from DESCRIBE but doesn't
    // expose the result yet. Once it provides this query (proposed name), the
    // settings page learns about hosts even when PyroWave wasn't requested.
#ifdef LI_PYROWAVE_HAS_HOST_CAPABILITIES_QUERY
    LI_PYROWAVE_CAPABILITIES hostCapabilities;
    if (LiGetHostPyrowaveCapabilities(&hostCapabilities)) {
        return PyrowaveHostOffer::Offered;
    }
    return PyrowaveHostOffer::NotOffered;
#else
    return PyrowaveHostOffer::Unknown;
#endif
}

QString PyrowaveVideoDecoder::launchErrorText(const QString& reason)
{
    return tr("PyroWave is selected as the video codec, but it can't be used on this PC: %1\n\n"
              "Choose another video codec in Settings to stream.")
        .arg(reason);
}

QString PyrowaveVideoDecoder::fallbackNotice(int negotiatedVideoFormat)
{
    return tr("PyroWave wasn't available from this host, so this stream uses %1. "
              "Your codec setting hasn't changed.")
        .arg(videoFormatDisplayName(negotiatedVideoFormat));
}

QString PyrowaveVideoDecoder::videoFormatDisplayName(int videoFormat)
{
    if (videoFormat == VIDEO_FORMAT_PYROWAVE) {
        return QStringLiteral("PyroWave");
    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        return QStringLiteral("AV1");
    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return QStringLiteral("HEVC");
    } else {
        return QStringLiteral("H.264");
    }
}

bool PyrowaveVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    SDL_assert(params->videoFormat == VIDEO_FORMAT_PYROWAVE);

    // PW-2 replaces this with the Vulkan decoder. Until then, refuse.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "PyroWave decoder requested (%dx%d, test-only: %d), but it isn't implemented",
                 params->width, params->height, m_TestOnly ? 1 : 0);
    return false;
}

QSize PyrowaveVideoDecoder::getDecoderMaxResolution()
{
    return QSize(k_MaxDimension, k_MaxDimension);
}

int PyrowaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT)
{
    // Never reached while initialize() fails. Dropping the frame is the
    // contract's answer for undecodable input; PyroWave has no IDR to request.
    return DR_OK;
}
