#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <atomic>
#include <optional>
#include <utility>
#include <vector>

using Uint32 = uint32_t;

template <typename T> const T& qMax(const T& a, const T& b)
{
    return a < b ? b : a;
}

struct QSize
{
    int w = -1;
    int h = -1;
    QSize() = default;
    QSize(int width, int height) : w(width), h(height) {}
    int width() const { return w; }
    int height() const { return h; }
    bool isValid() const { return w >= 0 && h >= 0; }
    QSize transposed() const { return QSize(h, w); }
    bool operator==(const QSize& other) const { return w == other.w && h == other.h; }
    bool operator!=(const QSize& other) const { return !(*this == other); }
};

namespace StreamQuality {
// LIVE_TRANSACTION_TYPES
}

struct SDL_Event
{
    int type = 0;
    struct
    {
        Uint32 windowID = 0;
        void* data1 = nullptr;
        void* data2 = nullptr;
    } user;
};

constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
constexpr int SDL_RENDER_DEVICE_RESET = 1;
constexpr int LI_RESOLUTION_CHANGE_OK = 0;
constexpr int LI_RESOLUTION_CHANGE_ERR_UNSUPPORTED = -2;
#define SDL_TICKS_PASSED(a, b) (static_cast<int32_t>((b) - (a)) <= 0)

Uint32 ticks = 1000;
QSize windowSize(1600, 900);
QSize lastRequest;
int sends = 0;
int sendResult = 0;
int resetEvents = 0;
int menuUpdates = 0;
bool queueFails = false;
bool decoderLocked = false;
bool destroyedUnderLock = false;

Uint32 SDL_GetTicks()
{
    return ticks;
}
#ifdef HARNESS_SDL_BEFORE_2_26
#define SDL_VERSION_ATLEAST(a, b, c) 0
void SDL_GetWindowSize(void*, int* width, int* height)
{
    *width = windowSize.width() / 2;
    *height = windowSize.height() / 2;
}
void SDL_GL_GetDrawableSize(void*, int* width, int* height)
{
    *width = windowSize.width();
    *height = windowSize.height();
}
#else
#define SDL_VERSION_ATLEAST(a, b, c) 1
void SDL_GetWindowSizeInPixels(void*, int* width, int* height)
{
    *width = windowSize.width();
    *height = windowSize.height();
}
#endif
void SDL_LogInfo(int, const char*, ...) {}
void SDL_LogWarn(int, const char*, ...) {}
int LiSendResolutionChangeRequest(int width, int height)
{
    ++sends;
    lastRequest = QSize(width, height);
    return sendResult;
}
int SDL_PushEvent(SDL_Event* event)
{
    if (queueFails) {
        return -1;
    }
    resetEvents += event->type == SDL_RENDER_DEVICE_RESET;
    return 1;
}
const char* SDL_GetError()
{
    return "test queue failure";
}
void SDL_LockMutex(void*)
{
    decoderLocked = true;
}
void SDL_UnlockMutex(void*)
{
    decoderLocked = false;
}

struct Decoder
{
    ~Decoder() { destroyedUnderLock = decoderLocked; }
};

namespace OverlayMenuPanel {
struct StreamConfigurationRequest
{
};
}

class Session
{
public:
    struct Preferences
    {
        bool resolutionMatchWindow = false;
    } preferences;
    Preferences* m_Preferences = &preferences;
    struct Config
    {
        int width = 1280;
        int height = 720;
    } m_StreamConfig;
    int m_ActiveVideoWidth = 1280;
    int m_ActiveVideoHeight = 720;
    void* m_Window = nullptr;
    void* m_DecoderLock = nullptr;
    Decoder* m_VideoDecoder = new Decoder;
    bool fullscreen = false;
    bool m_IsFullScreen = false;
    bool minimized = false;
    bool m_SuppressConnectionErrorDialog = false;
    bool m_PendingStreamQuality = false;
    std::optional<OverlayMenuPanel::StreamConfigurationRequest> m_PendingStreamConfiguration;
    std::atomic<Uint32> m_ConnectionGeneration{ 1 };
    StreamQuality::LiveResolutionTransaction m_LiveResolutionTransaction;
    // RESOLUTION_STATE
    ~Session() { delete m_VideoDecoder; }
    bool isStreamingWindowFullscreen() const { return fullscreen; }
    bool isStreamingWindowMinimized() const { return minimized; }
    void scheduleWindowResolutionMatch();
    void scheduleInitialWindowResolutionMatch(bool awaitingFullscreenEntry);
    bool requestWindowResolutionMatchNow();
    void processWindowResolutionMatch();
    void handleHostResolutionReport(int width, int height, Uint32 generation);
    void handleHostResolutionReport(int width, int height)
    {
        handleHostResolutionReport(width, height, m_ConnectionGeneration.load());
    }
    void updateStreamQualityMenuState() { ++menuUpdates; }
    void replayReports(const std::vector<SDL_Event>& events)
    {
        // RECONNECT_REPORT_DECLARATION
        for (const auto& ev : events) {
            // RECONNECT_REPORT_ENQUEUE
        }
        // RECONNECT_REPORT_REPLAY
    }
};

// SESSION_METHODS

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void prepare(Session& session, bool initialReport = true)
{
    ticks = 1000;
    sends = resetEvents = menuUpdates = 0;
    sendResult = 0;
    queueFails = destroyedUnderLock = false;
    windowSize = QSize(1600, 900);
    session.preferences.resolutionMatchWindow = true;
    session.m_ResolutionMatch.hostSupported = true;
    if (initialReport) {
        session.handleHostResolutionReport(1280, 720);
    }
    sends = resetEvents = menuUpdates = 0;
}

int main()
{
    bool ok = true;
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        ok &= require(session.requestWindowResolutionMatchNow(),
                      "Match now must work with automatic matching off");
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1600, 900) &&
                          !session.preferences.resolutionMatchWindow,
                      "one-shot must send exact size without saving automatic matching");
        ok &= require(!session.requestWindowResolutionMatchNow(),
                      "Match now must refuse an outstanding request");
        const Uint32 generation = session.m_ConnectionGeneration.load();
        session.handleHostResolutionReport(1600, 900, generation);
        session.m_LiveResolutionTransaction.decoderResetCompleted(generation);
        session.m_LiveResolutionTransaction.firstFrame(generation);
        ok &= require(!session.requestWindowResolutionMatchNow(),
                      "host acknowledgment must not bypass the rate limit");
        ticks += 400;
        ok &= require(session.requestWindowResolutionMatchNow(), "rate limit must expire");
    }
    {
        Session session;
        prepare(session);
        session.m_ResolutionMatch.hostSupported = false;
        ok &= require(!session.requestWindowResolutionMatchNow(),
                      "unsupported host must refuse Match now");
        session.m_ResolutionMatch.hostSupported = true;
        session.m_SuppressConnectionErrorDialog = true;
        ok &= require(!session.requestWindowResolutionMatchNow(),
                      "Match now must refuse during reconnect");
        session.m_SuppressConnectionErrorDialog = false;
        session.m_PendingStreamQuality = true;
        ok &= require(!session.requestWindowResolutionMatchNow(),
                      "Match now must not race a pending quality change");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.fullscreen = true;
        session.m_StreamConfig.width = 2560;
        session.m_StreamConfig.height = 1440;
        session.requestWindowResolutionMatchNow();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(2560, 1440),
                      "fullscreen one-shot must use configured stream size");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.m_ResolutionMatch.hostDisplaySize = QSize(1920, 1080);
        session.requestWindowResolutionMatchNow();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deferred,
                      "one-shot must preserve the 1:1 hot-switch gate");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "deferred one-shot must retry with preference still off");
    }
    {
        Session session;
        prepare(session);
        windowSize = QSize(1600, 900);
        session.scheduleInitialWindowResolutionMatch(false);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1600, 900),
                      "windowed startup must match the initial created window size");
    }
    {
        Session session;
        prepare(session);
        session.fullscreen = true;
        session.scheduleInitialWindowResolutionMatch(false);
        ok &= require(session.m_ResolutionMatch.deadline == 0,
                      "fullscreen startup must not schedule a window-size request");
    }
    {
        Session session;
        prepare(session);
        session.scheduleInitialWindowResolutionMatch(true);
        ok &= require(session.m_ResolutionMatch.deadline == 0,
                      "pending fullscreen entry must not match transient window bounds");
    }
    {
        Session session;
        prepare(session);
        // Fullscreen was requested but SDL rejected it, so actual window state
        // remains windowed and should still match its initial bounds.
        session.scheduleInitialWindowResolutionMatch(false);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1600, 900),
                      "failed fullscreen entry must match actual windowed bounds");
    }
    {
        Session session;
        prepare(session);
        session.m_ResolutionMatch.hostSupported = false;
        session.scheduleInitialWindowResolutionMatch(false);
        ok &= require(session.m_ResolutionMatch.deadline == 0,
                      "initial schedule must respect unsupported hosts");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.scheduleInitialWindowResolutionMatch(false);
        ok &= require(session.m_ResolutionMatch.deadline == 0,
                      "initial schedule must respect the disabled preference");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.m_ResolutionMatch.deadline = 1;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deadline == 0,
                      "disabled preference with a pending deadline must send nothing");
    }
    {
        Session session;
        prepare(session);
        session.m_ResolutionMatch.hostSupported = false;
        session.m_ResolutionMatch.deadline = 1;
        session.processWindowResolutionMatch();
        session.scheduleWindowResolutionMatch();
        session.handleHostResolutionReport(1600, 900);
        ok &= require(sends == 0 && session.m_ResolutionMatch.deadline == 0 && resetEvents == 1 &&
                          session.m_ActiveVideoWidth == 1600,
                      "unsupported hosts must follow display reports without sending requests");
    }
    {
        Session session;
        prepare(session);
        session.scheduleWindowResolutionMatch();
        ticks = 1399;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "resize must debounce for 400 ms");
        session.scheduleWindowResolutionMatch();
        ticks = 1799;
        windowSize = QSize(1601, 901);
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1600, 900),
                      "settled resize must send the latest even pixel size");
        windowSize = QSize(1920, 1080);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "an unanswered request must prevent another send");
        ok &= require(session.m_ResolutionMatch.deferred,
                      "a resize during a live transaction must remain deferred");
        ticks += 5000;
        session.m_LiveResolutionTransaction.expire(ticks);
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "an expired transaction must restart the resize debounce");
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 2 && lastRequest == windowSize, "request timeout must unblock");
        session.scheduleWindowResolutionMatch();
        ticks += 5500;
        session.m_LiveResolutionTransaction.expire(ticks);
        session.processWindowResolutionMatch();
        ok &= require(sends == 2, "an ignored request must not be sent repeatedly");
    }
    {
        Session session;
        prepare(session);
        sendResult = LI_RESOLUTION_CHANGE_ERR_UNSUPPORTED;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && !session.m_ResolutionMatch.hostSupported,
                      "unsupported response must disable subsequent requests");
    }
    {
        Session session;
        prepare(session);
        session.m_ResolutionMatch.hostDisplaySize = QSize(1920, 1080);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deferred,
                      "non-1:1 host display must defer rather than send");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "a 1:1 display report must retry the deferred resize");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.m_ResolutionMatch.hostDisplaySize = QSize();
        session.handleHostResolutionReport(1280, 720);
        ok &= require(resetEvents == 0 && session.m_VideoDecoder,
                      "first connection report only records display size");
        queueFails = true;
        session.handleHostResolutionReport(1600, 900);
        ok &= require(resetEvents == 0 && !session.m_VideoDecoder && destroyedUnderLock &&
                          session.m_ActiveVideoWidth == 1600,
                      "failed reset enqueue must still retire the old decoder and dimensions");
        queueFails = false;
        session.processWindowResolutionMatch();
        ok &= require(resetEvents == 1 && menuUpdates == 1 && !session.m_VideoDecoder &&
                          destroyedUnderLock && session.m_ActiveVideoWidth == 1600 &&
                          session.m_ActiveVideoHeight == 900,
                      "host-initiated changes must reset under lock even with matching off");
        session.processWindowResolutionMatch();
        ok &= require(resetEvents == 1, "a queued decoder reset must not be repeated");
    }
    {
        Session session;
        prepare(session);
        session.m_ActiveVideoWidth = 1600;
        session.m_ActiveVideoHeight = 900;
        session.m_ResolutionMatch.hostDisplaySize = QSize(1600, 900);
        session.fullscreen = true;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1280, 720),
                      "fullscreen must return to the configured resolution");
    }
    for (QSize invalid : { QSize(255, 900), QSize(1600, 16386) }) {
        Session session;
        prepare(session);
        windowSize = invalid;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "invalid dimensions must never send");
    }
    {
        Session session;
        prepare(session, false);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deferred,
                      "matching must wait for the connection's first display report");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "a 1:1 first report must retry the deferred match");
    }
    {
        Session session;
        prepare(session, false);
        session.handleHostResolutionReport(3840, 2160);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && resetEvents == 0,
                      "a scaled stream must start at its negotiated size without requests");
        session.handleHostResolutionReport(2560, 1440);
        ok &= require(resetEvents == 1 && session.m_ActiveVideoWidth == 854 &&
                          session.m_ActiveVideoHeight == 480,
                      "scaled reports must derive the encoded size rounded up to even");
        session.handleHostResolutionReport(150, 120);
        ok &= require(resetEvents == 2 && session.m_ActiveVideoWidth == 64 &&
                          session.m_ActiveVideoHeight == 64,
                      "scaled reports must clamp the encoded dimensions to 64");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "later display sizes must not change initial scaled eligibility");
    }
    {
        Session session;
        prepare(session, false);
        session.handleHostResolutionReport(1920, 1440);
        session.handleHostResolutionReport(1440, 1920);
        ok &= require(resetEvents == 1 && session.m_ActiveVideoWidth == 720 &&
                          session.m_ActiveVideoHeight == 1280,
                      "rotation must swap the initial per-axis scale");
    }
    {
        Session session;
        prepare(session, false);
        session.handleHostResolutionReport(720, 1280);
        ok &= require(resetEvents == 1 && session.m_ActiveVideoWidth == 720 &&
                          session.m_ActiveVideoHeight == 1280,
                      "a transposed first display report must reset the initial decoder");
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "transposed 1:1 streams must still allow requests");
    }
    {
        Session session;
        prepare(session, false);
        session.m_ConnectionGeneration = 2;
        std::vector<SDL_Event> events;
        for (const auto& report : { QSize(1920, 1080), QSize(3840, 2160), QSize(2560, 1440) }) {
            SDL_Event event;
            event.user.windowID = events.empty() ? 1 : 2;
            event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(report.width()));
            event.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(report.height()));
            events.push_back(event);
        }
        session.replayReports(events);
        ok &= require(resetEvents == 1 && session.m_ActiveVideoWidth == 854 &&
                          session.m_ActiveVideoHeight == 480 &&
                          session.m_ResolutionMatch.hostDisplaySize == QSize(2560, 1440),
                      "reconnect must replay every report in order and ignore stale generations");
    }
    return ok ? 0 : 1;
}
