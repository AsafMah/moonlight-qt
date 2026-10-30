#include "pyrowavevideodecoder.h"
#include "pyrowavepreflightstore.h"
#include "pyrowaveruntime.h"
#include "pyrowavevulkan.h"

#include "streaming/session.h"
#include "streaming/stats/videostatsbridge.h"
#include "streaming/streamutils.h"

#include <QSize>

#include <cstdarg>
#include <cstdio>

namespace {

// HOST_PROCESSING_LATENCY (frame envelope, section 3): big-endian uint16 in
// 0.1 ms units. Zero when this frame doesn't carry it, never the value of an
// earlier frame. common-c has already validated the TLV area.
uint16_t hostProcessingLatency(const uint8_t* metadata, size_t length)
{
    size_t offset = 0;
    while (metadata != nullptr && length - offset >= 8) {
        const uint8_t* entry = metadata + offset;
        const uint16_t type = (uint16_t)((entry[0] << 8) | entry[1]);
        const uint32_t valueLength = ((uint32_t)entry[4] << 24) | ((uint32_t)entry[5] << 16) |
                                     ((uint32_t)entry[6] << 8) | (uint32_t)entry[7];
        offset += 8;
        if (valueLength > length - offset) {
            break;
        }
        if (type == LI_PYROWAVE_METADATA_HOST_PROCESSING_LATENCY && valueLength == 2) {
            return (uint16_t)((metadata[offset] << 8) | metadata[offset + 1]);
        }
        offset += valueLength;
    }
    return 0;
}

// Dynamic range label for the overlay and the stats model's codec name
const char* dynamicRangeLabel(int hdrMode)
{
    return hdrMode == 2 ? "HLG" : hdrMode == 1 ? "HDR10" : "SDR";
}

void appendFormatted(char* output, int length, int& offset, const char* format, ...)
{
    if (offset >= length) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(&output[offset], length - offset, format, args);
    va_end(args);
    if (written > 0) {
        offset = SDL_min(length - 1, offset + written);
    }
}

// The same accumulation as FFmpegVideoDecoder::addVideoStats(), which stays
// as upstream wrote it so merges from there stay clean.
void addVideoStats(const VIDEO_STATS& src, VIDEO_STATS& dst)
{
    dst.receivedFrames += src.receivedFrames;
    dst.decodedFrames += src.decodedFrames;
    dst.renderedFrames += src.renderedFrames;
    dst.totalFrames += src.totalFrames;
    dst.networkDroppedFrames += src.networkDroppedFrames;
    dst.pacerDroppedFrames += src.pacerDroppedFrames;
    dst.totalReassemblyTimeUs += src.totalReassemblyTimeUs;
    dst.totalDecodeTimeUs += src.totalDecodeTimeUs;
    dst.totalPacerTimeUs += src.totalPacerTimeUs;
    dst.totalRenderTimeUs += src.totalRenderTimeUs;

    if (dst.minHostProcessingLatency == 0) {
        dst.minHostProcessingLatency = src.minHostProcessingLatency;
    } else if (src.minHostProcessingLatency != 0) {
        dst.minHostProcessingLatency =
            SDL_min(dst.minHostProcessingLatency, src.minHostProcessingLatency);
    }
    dst.maxHostProcessingLatency =
        SDL_max(dst.maxHostProcessingLatency, src.maxHostProcessingLatency);
    dst.totalHostProcessingLatency += src.totalHostProcessingLatency;
    dst.framesWithHostProcessingLatency += src.framesWithHostProcessingLatency;

    if (!LiGetEstimatedRttInfo(&dst.lastRtt, &dst.lastRttVariance)) {
        dst.lastRtt = 0;
        dst.lastRttVariance = 0;
    }

    if (!dst.measurementStartUs) {
        dst.measurementStartUs = src.measurementStartUs;
    }

    const double timeDiffSecs = (double)(LiGetMicroseconds() - dst.measurementStartUs) / 1000000.0;
    if (timeDiffSecs > 0) {
        dst.totalFps = (double)dst.totalFrames / timeDiffSecs;
        dst.receivedFps = (double)dst.receivedFrames / timeDiffSecs;
        dst.decodedFps = (double)dst.decodedFrames / timeDiffSecs;
        dst.renderedFps = (double)dst.renderedFrames / timeDiffSecs;
    }
}

}

PyrowaveVideoDecoder::PyrowaveVideoDecoder(bool testOnly, const PyrowaveSessionContract& contract)
    : m_TestOnly(testOnly), m_Contract(contract)
{
    SDL_AtomicSet(&m_DecoderThreadShouldQuit, 0);
}

PyrowavePreflightResult PyrowaveVideoDecoder::preflight(const PyrowavePreflightRequest& request)
{
    const PyrowavePreflightResult result = runPreflight(request);

    PyrowavePreflightStore::Entry entry;
    entry.ok = result.ok;
    entry.gpu = result.deviceName;
    entry.decodePath = result.decodePath;
    entry.reason = result.reason;
    entry.width = request.width;
    entry.height = request.height;
    entry.frameRate = request.frameRate;
    entry.hdrMode = request.hdrMode;
    PyrowavePreflightStore::record(entry);
    return result;
}

PyrowavePreflightResult PyrowaveVideoDecoder::runPreflight(const PyrowavePreflightRequest& request)
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

    // HDR10 and HLG need HDR output on this GPU and window, which the
    // renderer below checks. When it can't, the launch fails with the reason;
    // SDR is never streamed in HDR's place.
    if (!PyrowaveColor::Contract(request.hdrMode, false).isValid()) {
        result.reason =
            tr("PyroWave doesn't support the selected HDR mode (%1).").arg(request.hdrMode);
        return result;
    }

#ifndef HAVE_LIBPLACEBO_VULKAN
    result.reason = tr("This build has no Vulkan renderer, which PyroWave needs.");
    return result;
#else
    // Build everything a stream would, on the window the stream will use, and
    // release it again. The renderer is declared second so it goes first.
    PyrowaveFailure failure;
    PyrowaveVulkanInstance instance;
    if (!instance.initialize(request.window, failure)) {
        result.reason = failure.reason;
        return result;
    }

    PyrowaveVulkanRenderer renderer(instance);
    PyrowavePresentParams params;
    params.width = request.width;
    params.height = request.height;
    params.hdrMode = request.hdrMode;
    if (!renderer.initialize(params, failure)) {
        result.reason = failure.reason;
        result.deviceName = QString::fromStdString(renderer.deviceName());

        // Say so when only HDR is the problem. The same GPU and window are
        // tried in SDR just to tell, never to stream SDR instead.
        if (request.hdrMode != 0) {
            PyrowaveVulkanRenderer sdrRenderer(instance);
            PyrowavePresentParams sdrParams = params;
            sdrParams.hdrMode = 0;
            PyrowaveFailure sdrFailure;
            if (sdrRenderer.initialize(sdrParams, sdrFailure)) {
                result.reason = tr("This PC can't show PyroWave in HDR: %1 Turn off HDR in "
                                   "Settings to stream with PyroWave in SDR.")
                                    .arg(failure.reason);
            }
        }
        return result;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave preflight passed on '%s' (%dx%d, %s, %s)",
                renderer.deviceName().c_str(), request.width, request.height,
                PyrowaveColor::Contract(request.hdrMode, false).signalName(),
                qUtf8Printable(renderer.decodePathName()));
    result.ok = true;
    result.deviceName = QString::fromStdString(renderer.deviceName());
    result.decodePath = renderer.decodePathName();
    return result;
#endif
}

int PyrowaveVideoDecoder::advertisedVideoFormats(int supportedVideoFormats)
{
    if (!k_DecoderImplemented) {
        return supportedVideoFormats;
    }

    // moonlight-common-c tries PyroWave first and falls back to the legacy
    // formats in supportedVideoFormats (what Automatic would pick, in 4:2:0).
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

    // The host's DESCRIBE offer, reported even when another codec was
    // negotiated, so the settings page learns about hosts even when PyroWave
    // wasn't requested. Older moonlight-common-c builds lack the query.
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

QString PyrowaveVideoDecoder::fatalErrorText(int stage, const QString& reason)
{
    QString stageName;
    switch (static_cast<PyrowaveStage>(stage)) {
    case PyrowaveStage::Create:
        stageName = tr("setting up the decoder");
        break;
    case PyrowaveStage::Decode:
        stageName = tr("decoding");
        break;
    case PyrowaveStage::Sync:
        stageName = tr("GPU synchronization");
        break;
    case PyrowaveStage::Color:
        stageName = tr("checking the color format");
        break;
    case PyrowaveStage::Present:
        stageName = tr("presenting frames");
        break;
    }

    return tr("The PyroWave stream stopped after %1 failures in a row while %2: %3\n\n"
              "Choose another video codec in Settings if this keeps happening.")
        .arg(k_MaxConsecutiveFailures)
        .arg(stageName, reason);
}

PyrowaveVideoDecoder::~PyrowaveVideoDecoder()
{
    if (!m_TestOnly) {
        Session::get()->getOverlayManager().setOverlayRenderer(nullptr);
    }

    if (m_DecoderThread != nullptr) {
        SDL_AtomicSet(&m_DecoderThreadShouldQuit, 1);
        LiWakeWaitForVideoFrame();
        SDL_WaitThread(m_DecoderThread, nullptr);
        m_DecoderThread = nullptr;
    }

#ifdef HAVE_LIBPLACEBO_VULKAN
    {
        std::lock_guard<std::mutex> lock(m_RendererLock);
        m_Renderer.reset();
    }
    m_Instance.reset();
#endif

    if (!m_TestOnly && m_GlobalVideoStats.renderedFrames != 0) {
        char statsText[1024];
        stringifyVideoStats(m_GlobalVideoStats, statsText, sizeof(statsText));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Global video stats");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "----------------------------------------------------------\n%s", statsText);
    }
}

bool PyrowaveVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    SDL_assert(params->videoFormat == VIDEO_FORMAT_PYROWAVE);

#ifndef HAVE_LIBPLACEBO_VULKAN
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave needs a build with libplacebo Vulkan");
    return false;
#else
    m_Width = params->width;
    m_Height = params->height;
    m_StreamFps = params->frameRate;
    m_Window = params->window;
    m_EnableVsync = params->enableVsync;
    m_IgnoreAspectRatio = params->ignoreAspectRatio;

    // enableFramePacing is only set with V-Sync on
    m_EnableFramePacing = params->enableFramePacing;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(params->window);
    m_PacerDisplayFps = m_DisplayFps;
    m_FramePacer.configure(m_EnableFramePacing, m_StreamFps, m_PacerDisplayFps);
    // VRR turns V-Sync and frame pacing off (see VrrPolicy::decide()), so the
    // swapchain presents without waiting; only one refresh between presents
    // is left to enforce here
    m_VrrLimiter = VrrPolicy::FrameLimiter::forDisplay(params->enableVrr ? m_DisplayFps.load() : 0);
    if (params->enableVrr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave VRR mode: presenting on arrival, at least one refresh apart "
                    "(%d Hz display)",
                    m_DisplayFps.load());
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave pacing: %d FPS stream, %d Hz display, %s",
                m_StreamFps, m_PacerDisplayFps,
                m_FramePacer.isDecimating() ? "presenting an even share of frames"
                : m_EnableFramePacing       ? "every frame"
                                            : "frame pacing off");

    // preflight() refused anything else, so this can't happen.
    if (!PyrowaveColor::Contract(m_Contract.hdrMode, false).isValid()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave doesn't support HDR mode %d",
                     m_Contract.hdrMode);
        return false;
    }

    PyrowaveFailure failure;
    m_Instance = std::make_unique<PyrowaveVulkanInstance>();
    if (!m_Instance->initialize(params->window, failure)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave Vulkan instance failed: %s",
                     qUtf8Printable(failure.reason));
        m_Instance.reset();
        if (m_TestOnly) {
            return false;
        }

        // The surface needs the main thread, so this can't be retried from
        // the decode thread. End the stream with the reason instead of the
        // generic decoder error.
        m_ConsecutiveFailures = k_MaxConsecutiveFailures - 1;
        noteFailure(failure);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_RendererLock);
        if (!createRenderer(failure)) {
            if (m_TestOnly) {
                return false;
            }

            // Counts as the first failure; the next frames retry.
            noteFailure(failure);
        }
    }

    if (!m_TestOnly) {
        Session::get()->getOverlayManager().setOverlayRenderer(this);

        if (m_Contract.pullRenderer) {
            m_DecoderThread = SDL_CreateThread(decoderThreadProc, "PyroWave", this);
            if (m_DecoderThread == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to start the PyroWave thread: %s", SDL_GetError());
                return false;
            }
        }
    }

    return true;
#endif
}

#ifdef HAVE_LIBPLACEBO_VULKAN

bool PyrowaveVideoDecoder::createRenderer(PyrowaveFailure& failure)
{
    PyrowavePresentParams params;
    params.width = m_Width;
    params.height = m_Height;
    params.fullRange = m_Contract.colorRange == COLOR_RANGE_FULL;
    params.hdrMode = m_Contract.hdrMode;
    params.enableVsync = m_EnableVsync;
    params.ignoreAspectRatio = m_IgnoreAspectRatio;
    params.frameRate = m_StreamFps;
    params.displayFps = m_DisplayFps;

    auto renderer = std::make_unique<PyrowaveVulkanRenderer>(*m_Instance);
    {
        // The swapchain is first hinted during initialize(), so it gets the
        // metadata the session already has.
        std::lock_guard<std::mutex> hdrLock(m_HdrLock);
        renderer->setHdrSnapshot(m_HdrSnapshot);
    }
    if (!renderer->initialize(params, failure)) {
        failure.stage = PyrowaveStage::Create;
        return false;
    }

    if (m_Contract.hdrMode != 0) {
        m_HdrVerified = true;
    }
    m_DecoderName = renderer->decoderDescription();
    m_RendererName = QStringLiteral("libplacebo Vulkan (%1)")
                         .arg(QString::fromStdString(renderer->deviceName()));
    m_Renderer = std::move(renderer);
    return true;
}

void PyrowaveVideoDecoder::noteFailure(const PyrowaveFailure& failure)
{
    m_ConsecutiveFailures++;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave %s failure (%d of %d): %s",
                 pyrowaveStageName(failure.stage), m_ConsecutiveFailures, k_MaxConsecutiveFailures,
                 qUtf8Printable(failure.reason));

    if (m_ConsecutiveFailures < k_MaxConsecutiveFailures || m_Fatal) {
        return;
    }

    // Bounded recovery is used up. End the connection with the stage and
    // reason; never switch to another codec in this stream.
    m_Fatal = true;
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_PYROWAVE_FATAL;
    event.user.data1 = (void*)(intptr_t)failure.stage;
    event.user.data2 = new QString(failure.reason);
    if (SDL_PushEvent(&event) <= 0) {
        delete static_cast<QString*>(event.user.data2);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to report the PyroWave failure: %s",
                     SDL_GetError());
    }
}

#endif

int PyrowaveVideoDecoder::decoderThreadProc(void* context)
{
    auto me = static_cast<PyrowaveVideoDecoder*>(context);
    while (!SDL_AtomicGet(&me->m_DecoderThreadShouldQuit)) {
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        if (!LiWaitForNextVideoFrame(&handle, &du)) {
            // This might be a signal from the main thread to exit
            continue;
        }

        LiCompleteVideoFrame(handle, me->processFrame(du));
    }
    return 0;
}

int PyrowaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)
{
    // Push model: called on moonlight-common-c's decoder thread.
    return processFrame(du);
}

void PyrowaveVideoDecoder::accountReceivedFrame(PDECODE_UNIT du)
{
    if (!m_LastFrameNumber) {
        m_ActiveWndVideoStats.measurementStartUs = LiGetMicroseconds();
        m_LastFrameNumber = du->frameNumber;
    } else {
        // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
        m_ActiveWndVideoStats.networkDroppedFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_ActiveWndVideoStats.totalFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_LastFrameNumber = du->frameNumber;
    }

    // Flip stats windows roughly every second
    if (LiGetMicroseconds() > m_ActiveWndVideoStats.measurementStartUs + 1000000) {
        VIDEO_STATS lastTwoWndStats = {};
        addVideoStats(m_LastWndVideoStats, lastTwoWndStats);
        addVideoStats(m_ActiveWndVideoStats, lastTwoWndStats);

        // Publish to the stats model first, since the overlay text reads it
        // back, as FFmpegVideoDecoder does.
        m_BandwidthMbps =
            m_BandwidthMeter.update(LiGetRTPVideoBytesReceived(), LiGetMicroseconds());
        publishStatsWindow(lastTwoWndStats, m_ActiveWndVideoStats);

        Overlay::OverlayManager& overlays = Session::get()->getOverlayManager();
        if (overlays.isOverlayEnabled(Overlay::OverlayDebug)) {
            stringifyVideoStats(lastTwoWndStats, overlays.getOverlayText(Overlay::OverlayDebug),
                                overlays.getOverlayMaxTextLength());
            overlays.setOverlayTextUpdated(Overlay::OverlayDebug);
        }

        addVideoStats(m_ActiveWndVideoStats, m_GlobalVideoStats);
        m_LastWndVideoStats = m_ActiveWndVideoStats;
        m_ActiveWndVideoStats = {};
        m_ActiveWndVideoStats.measurementStartUs = LiGetMicroseconds();
    }

    // From the frame's own TLV, so it is never carried over from another frame.
    const uint16_t latency =
        hostProcessingLatency(du->pyrowaveMetadata, du->pyrowaveMetadataLength);
    if (latency != 0) {
        if (m_ActiveWndVideoStats.minHostProcessingLatency != 0) {
            m_ActiveWndVideoStats.minHostProcessingLatency =
                SDL_min(m_ActiveWndVideoStats.minHostProcessingLatency, latency);
        } else {
            m_ActiveWndVideoStats.minHostProcessingLatency = latency;
        }
        m_ActiveWndVideoStats.framesWithHostProcessingLatency += 1;
        m_ActiveWndVideoStats.maxHostProcessingLatency =
            SDL_max(m_ActiveWndVideoStats.maxHostProcessingLatency, latency);
        m_ActiveWndVideoStats.totalHostProcessingLatency += latency;
    }

    m_ActiveWndVideoStats.receivedFrames++;
    m_ActiveWndVideoStats.totalFrames++;
    m_ActiveWndVideoStats.totalReassemblyTimeUs += (du->enqueueTimeUs - du->receiveTimeUs);

    Session::get()->getStatsModel().recordFrameArrival(du->receiveTimeUs, du->rtpTimestamp);
}

void PyrowaveVideoDecoder::publishStatsWindow(const VIDEO_STATS& recent,
                                              const VIDEO_STATS& completed)
{
    StatsWindow window = VideoStatsBridge::toStatsWindow(recent);
    window.queueMeasured = false;
    window.pacerDropEligibleFrames = recent.decodedFrames + recent.pacerDroppedFrames;

    window.width = m_Width;
    window.height = m_Height;
    window.targetFps = m_StreamFps;
    window.codec =
        QStringLiteral("PyroWave %1").arg(QLatin1String(dynamicRangeLabel(m_Contract.hdrMode)));
    window.decoder = m_DecoderName;
    window.renderer = m_RendererName;
    window.hardwareDecode = true;
    window.bitrateMbps = m_BandwidthMbps;
    VideoStatsBridge::readRtpCounters(window);

    StatsWindow completedWindow = VideoStatsBridge::toStatsWindow(completed);
    completedWindow.queueMeasured = false;
    completedWindow.pacerDropEligibleFrames =
        completed.decodedFrames + completed.pacerDroppedFrames;
    Session::get()->getStatsModel().publishWindow(window, completedWindow, LiGetMicroseconds());
}

int PyrowaveVideoDecoder::processFrame(PDECODE_UNIT du)
{
    SDL_assert(!m_TestOnly);

    accountReceivedFrame(du);

#ifndef HAVE_LIBPLACEBO_VULKAN
    return DR_OK;
#else
    if (m_Fatal) {
        // The stream is ending; see noteFailure().
        return DR_OK;
    }

    // Every PyroWave frame is intra-coded, so when newer frames are already
    // queued this one can be skipped without harming them. That keeps a slow
    // present (V-Sync, a busy GPU) from turning into latency.
    if (LiGetPendingVideoFrames() > 0) {
        m_ActiveWndVideoStats.pacerDroppedFrames++;
        return DR_OK;
    }

    // Frame pacing follows the display the window is on now.
    const int displayFps = m_DisplayFps;
    if (displayFps != m_PacerDisplayFps) {
        m_PacerDisplayFps = displayFps;
        m_FramePacer.configure(m_EnableFramePacing, m_StreamFps, displayFps);
    }
    if (!m_FramePacer.shouldPresent()) {
        m_ActiveWndVideoStats.pacerDroppedFrames++;
        return DR_OK;
    }

    // VRR: wait for the present-rate cap. A newer frame arriving meanwhile
    // replaces this one, as above.
    if (const uint64_t waitUs = m_VrrLimiter.waitUs(LiGetMicroseconds()); waitUs > 0) {
        SDL_Delay((Uint32)((waitUs + 999) / 1000));
        if (LiGetPendingVideoFrames() > 0) {
            m_ActiveWndVideoStats.pacerDroppedFrames++;
            return DR_OK;
        }
    }

    // The decoder takes the payload in one piece.
    m_FrameBuffer.resize((size_t)du->fullLength);
    size_t offset = 0;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        if (offset + (size_t)entry->length > m_FrameBuffer.size()) {
            m_FrameBuffer.resize(offset + (size_t)entry->length);
        }
        memcpy(&m_FrameBuffer[offset], entry->data, (size_t)entry->length);
        offset += (size_t)entry->length;
    }

    PyrowaveFailure failure;
    PyrowaveFrameTimings timings;
    PyrowaveVulkanRenderer::FrameResult result = PyrowaveVulkanRenderer::FrameResult::Failed;
    bool rebuilt = false;
    {
        std::lock_guard<std::mutex> lock(m_RendererLock);

        // A failure discarded the renderer; build a new one on the same
        // window. That is the bounded rebuild, counted by noteFailure().
        if (m_Renderer == nullptr) {
            rebuilt = createRenderer(failure);
        }
        if (m_Renderer != nullptr) {
            {
                std::lock_guard<std::mutex> hdrLock(m_HdrLock);
                m_Renderer->setHdrSnapshot(m_HdrSnapshot);
            }
            result = m_Renderer->submitFrame(m_FrameBuffer.data(), offset, timings, failure);
            if (result == PyrowaveVulkanRenderer::FrameResult::Failed) {
                m_Renderer.reset();
            }
        }
    }

    // The new renderer starts without overlay textures. Ask for them again,
    // outside the lock: this calls back into notifyOverlayUpdated().
    if (rebuilt) {
        Overlay::OverlayManager& overlays = Session::get()->getOverlayManager();
        for (int i = 0; i < Overlay::OverlayMax; i++) {
            if (overlays.isOverlayEnabled((Overlay::OverlayType)i)) {
                overlays.setOverlayTextUpdated((Overlay::OverlayType)i);
            }
        }
    }

    switch (result) {
    case PyrowaveVulkanRenderer::FrameResult::Presented:
        // Present times feed the frame time percentiles (1% and 0.1% lows)
        Session::get()->getStatsModel().recordFramePresented(LiGetMicroseconds());
        m_VrrLimiter.onPresented(LiGetMicroseconds());

        // Only a decoded and presented frame clears the failure count.
        m_ConsecutiveFailures = 0;
        m_ActiveWndVideoStats.decodedFrames++;
        m_ActiveWndVideoStats.renderedFrames++;
        m_ActiveWndVideoStats.totalDecodeTimeUs += timings.decodeUs;
        m_ActiveWndVideoStats.totalRenderTimeUs += timings.renderUs;
        break;
    case PyrowaveVulkanRenderer::FrameResult::Dropped:
        // Packets the decoder needed are missing: a network loss, not a fault.
        m_ActiveWndVideoStats.networkDroppedFrames++;
        break;
    case PyrowaveVulkanRenderer::FrameResult::NotPresented:
        // The window can't show anything right now. Not a failure.
        m_ActiveWndVideoStats.pacerDroppedFrames++;
        break;
    case PyrowaveVulkanRenderer::FrameResult::Failed:
        noteFailure(failure);
        break;
    }

    // PyroWave has no reference frames, so there is never an IDR to request.
    return DR_OK;
#endif
}

int PyrowaveVideoDecoder::getDecoderColorspace()
{
    return m_Contract.hdrMode != 0 ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
}

void PyrowaveVideoDecoder::setHdrMode(bool enabled)
{
    if (m_TestOnly || m_Contract.hdrMode == 0) {
        // An SDR stream stays SDR whatever the host's display does.
        return;
    }

    SS_HDR_METADATA raw = {};
    const bool haveMetadata = enabled && LiGetHdrMetadata(&raw);

    PyrowaveColor::HdrSnapshot::Update update;
    bool valid;
    {
        std::lock_guard<std::mutex> lock(m_HdrLock);
        update = m_HdrSnapshot.update(enabled, haveMetadata ? &raw : nullptr);
        valid = m_HdrSnapshot.valid();
    }

    // The stream keeps its HDR signal either way. Without valid metadata the
    // frames are shown without mastering data instead of invented values.
    switch (update) {
    case PyrowaveColor::HdrSnapshot::Update::Accepted:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave HDR metadata: max %u nits, min %.4f nits, MaxCLL %u, MaxFALL %u",
                    (unsigned)raw.maxDisplayLuminance, raw.minDisplayLuminance / 10000.0,
                    (unsigned)raw.maxContentLightLevel, (unsigned)raw.maxFrameAverageLightLevel);
        break;
    case PyrowaveColor::HdrSnapshot::Update::Cleared:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave host HDR mode is off; frames stay HDR without mastering metadata");
        break;
    case PyrowaveColor::HdrSnapshot::Update::Missing:
    case PyrowaveColor::HdrSnapshot::Update::Invalid:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave HDR metadata %s; %s",
                    PyrowaveColor::updateName(update),
                    valid ? "keeping the previous snapshot" : "none applied");
        break;
    }
}

void PyrowaveVideoDecoder::notifyOverlayUpdated(Overlay::OverlayType type)
{
#ifdef HAVE_LIBPLACEBO_VULKAN
    std::lock_guard<std::mutex> lock(m_RendererLock);
    if (m_Renderer != nullptr) {
        m_Renderer->notifyOverlayUpdated(type);
    }
#else
    Q_UNUSED(type);
#endif
}

bool PyrowaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    if ((info->stateChangeFlags & WINDOW_STATE_CHANGE_DISPLAY) && m_Window != nullptr) {
        m_DisplayFps = StreamUtils::getDisplayRefreshRate(m_Window);
    }

    // The swapchain follows size changes on the next frame, like PlVkRenderer
    return !(info->stateChangeFlags & ~(WINDOW_STATE_CHANGE_SIZE | WINDOW_STATE_CHANGE_DISPLAY));
}

QSize PyrowaveVideoDecoder::getDecoderMaxResolution()
{
    return QSize(k_MaxDimension, k_MaxDimension);
}

void PyrowaveVideoDecoder::stringifyVideoStats(const VIDEO_STATS& stats, char* output,
                                               int length) const
{
    // The same layout as FFmpegVideoDecoder's overlay text, sharing its last
    // line. FEC results are in the stats overlay (StatsSnapshot), which this
    // text doesn't show for any codec.
    int offset = 0;
    output[0] = 0;

    if (stats.receivedFps > 0) {
        appendFormatted(output, length, offset,
                        " {18}%dx%d@%.0f    PyroWave %s   {18}FPS  %.1f {14}Rx{18} · %.1f "
                        "{14}De{18} · %.1f {14}Rd{18} \n",
                        m_Width, m_Height, stats.totalFps, dynamicRangeLabel(m_Contract.hdrMode),
                        stats.receivedFps, stats.decodedFps, stats.renderedFps);
    }

    if (stats.renderedFrames != 0) {
        char rttString[32];
        if (stats.lastRtt != 0) {
            snprintf(rttString, sizeof(rttString), "**%u** ± %ums", stats.lastRtt,
                     stats.lastRttVariance);
        } else {
            snprintf(rttString, sizeof(rttString), "N/A");
        }

        char bandwidthString[32];
        if (!std::isnan(m_BandwidthMbps)) {
            snprintf(bandwidthString, sizeof(bandwidthString), "**%.2f** {16}Mbps{18}",
                     m_BandwidthMbps);
        } else {
            snprintf(bandwidthString, sizeof(bandwidthString), "N/A");
        }

        appendFormatted(
            output, length, offset,
            " Network %s  Loss %.2f%%  Bandwidth %s  {16}|  {18}Render **%.2f**ms · Decode "
            "**%.2f**ms ",
            rttString,
            stats.totalFrames ? (float)stats.networkDroppedFrames / stats.totalFrames * 100 : 0.0f,
            bandwidthString, (double)stats.totalRenderTimeUs / 1000.0 / stats.renderedFrames,
            stats.decodedFrames ? (double)stats.totalDecodeTimeUs / 1000.0 / stats.decodedFrames
                                : 0.0);
    }

    if (stats.framesWithHostProcessingLatency > 0) {
        appendFormatted(output, length, offset, "· Host processing **%.1f**ms ",
                        (float)stats.totalHostProcessingLatency / 10 /
                            stats.framesWithHostProcessingLatency);
    }

    // Frame rate lows, jitter and the latency estimate from the stats model.
    // The global stats logged at the end get whole-session lows.
    if (stats.renderedFrames != 0 && Session::get() != nullptr && offset < length) {
        const StatsSnapshot snapshot =
            Session::get()->getStatsModel().snapshot(LiGetMicroseconds());
        const int written = VideoStatsBridge::formatOverlayLows(
            snapshot, stats, &stats == &m_GlobalVideoStats, &output[offset], length - offset);
        if (written > 0) {
            offset = SDL_min(length - 1, offset + written);
        }
    }
}
