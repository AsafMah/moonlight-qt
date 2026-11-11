#pragma once

#include <Limelight.h>
#include "SDL_compat.h"
#include "settings/streamingpreferences.h"

#define SDL_CODE_FRAME_READY 0
// Pacer: presents are held below the stream rate by the driver or an external
// limiter. data1 = achieved present FPS, data2 = stream FPS.
#define SDL_CODE_PRESENT_THROTTLE_NOTICE 300
// Renderer: VRR engagement verdict changed in a way the session acts on
// (vrrengagementdetector.h). data1 = Verdict, data2 = Source.
#define SDL_CODE_VRR_ENGAGEMENT_NOTICE 301

#define MAX_SLICES 4

typedef struct _VIDEO_STATS {
    uint32_t receivedFrames;
    uint32_t decodedFrames;
    uint32_t renderedFrames;
    uint32_t totalFrames;
    uint32_t networkDroppedFrames;
    uint32_t pacerDroppedFrames;
    uint32_t localDroppedFrames;               // complete frames moonlight-common-c discarded
    uint32_t hostSkippedFrames;                // never sent by the host; not in totalFrames
    uint16_t minHostProcessingLatency;         // low-res from RTP
    uint16_t maxHostProcessingLatency;         // low-res from RTP
    uint32_t totalHostProcessingLatency;       // low-res from RTP
    uint32_t framesWithHostProcessingLatency;  // low-res from RTP
    uint64_t totalReassemblyTimeUs;            // high-res (1us)
    uint64_t totalDecodeTimeUs;                // high-res (1us)
    uint64_t totalPacerTimeUs;                 // high-res (1us)
    uint64_t totalRenderTimeUs;                // high-res (1us)
    uint32_t timedRenderFrames;                // successful presents with phase timing
    uint64_t totalPrePresentTimeUs;            // high-res (1us)
    uint64_t totalPresentTimeUs;               // high-res (1us)
    uint32_t lastRtt;                          // low-res from enet (1ms)
    uint32_t lastRttVariance;                  // low-res from enet (1ms)
    double totalFps;                           // high-res
    double receivedFps;                        // high-res
    double decodedFps;                         // high-res
    double renderedFps;                        // high-res
    double videoMegabitsPerSec;                // current video bitrate in Mbps, not including FEC overhead
    uint64_t measurementStartUs;               // microseconds
} VIDEO_STATS, *PVIDEO_STATS;

typedef struct _DECODER_PARAMETERS {
    SDL_Window* window;
    StreamingPreferences::VideoDecoderSelection vds;
    StreamingPreferences::RendererSelection renderer;

    int videoFormat;
    int width;
    int height;
    int frameRate;
    bool enableVsync;
    // VRR (G-Sync / FreeSync) mode: V-Sync and frame pacing are off, frames
    // are presented on arrival with tearing allowed, and Pacer caps the
    // present rate just below the refresh rate
    bool enableVrr;
    // Which VRR pacer runs when enableVrr is set. The Nonary pacer (ported
    // from Nonary/moonlight-qt) keeps V-Sync on at the session boundary and
    // schedules each frame from its host timestamp; renderers without its
    // presenter fall back to the fork's limiter.
    StreamingPreferences::VrrImplementation vrrImplementation;
    // Nonary pacer: 0 Smooth, 1 Balanced, 2 Low latency
    int vrrLatencyMode;
    // Nonary pacer: refresh rate snapshotted by the session, 0 if unknown
    int vrrDisplayRefreshHz;
    // VRR Smooth mode: present each frame at its sender timestamp plus a
    // learned playout delay instead of on arrival
    bool smoothVrr = false;
    // Whether the renderer checks that VRR is engaged on the display:
    // 0 off, 1 in VRR mode (notice when VRR is not engaged), 2 in the
    // fixed-refresh fallback for a VRR session (notice when G-Sync is
    // active again)
    int vrrEngagementMonitor = 0;
    bool enableFramePacing;
    bool enableVideoEnhancement;
    bool ignoreAspectRatio;
    bool testOnly;
} DECODER_PARAMETERS, *PDECODER_PARAMETERS;

#define WINDOW_STATE_CHANGE_SIZE 0x01
#define WINDOW_STATE_CHANGE_DISPLAY 0x02
// Used by the Nonary VRR pacer to discard queued work around window transitions
#define WINDOW_STATE_CHANGE_MINIMIZED 0x04
#define WINDOW_STATE_CHANGE_RESTORED 0x08
#define WINDOW_STATE_CHANGE_SUSPENDED 0x10

typedef struct _WINDOW_STATE_CHANGE_INFO {
    SDL_Window* window;
    uint32_t stateChangeFlags;

    // Populated if WINDOW_STATE_CHANGE_SIZE is set
    int width;
    int height;

    // Populated if WINDOW_STATE_CHANGE_DISPLAY is set
    int displayIndex;
} WINDOW_STATE_CHANGE_INFO, *PWINDOW_STATE_CHANGE_INFO;

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() {}
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool isHardwareAccelerated() = 0;
    virtual bool isAlwaysFullScreen() = 0;
    virtual bool isHdrSupported() = 0;
    virtual int getDecoderCapabilities() = 0;
    virtual int getDecoderColorspace() = 0;
    virtual int getDecoderColorRange() = 0;
    virtual QSize getDecoderMaxResolution() = 0;
    virtual int submitDecodeUnit(PDECODE_UNIT du) = 0;
    virtual void renderFrameOnMainThread() = 0;
    virtual void setHdrMode(bool enabled) = 0;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) = 0;
    // True when the Nonary VRR pacer owns presentation for this decoder
    virtual bool isNonaryVrrActive() { return false; }
};
