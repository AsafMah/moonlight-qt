#include <QSize>
#include <QString>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <vector>
#include "recoverybudget.h"

using Uint32 = uint32_t;
using Uint64 = uint64_t;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
constexpr int SDL_QUIT = 1, SDL_WINDOWEVENT = 2, SDL_KEYDOWN = 3, SDL_USEREVENT = 4;
constexpr int SDL_CONTROLLERDEVICEADDED = 5, SDL_CONTROLLERDEVICEREMOVED = 6;
constexpr int SDL_RENDER_DEVICE_RESET = 7, SDL_WINDOWEVENT_CLOSE = 8, SDLK_ESCAPE = 9;
constexpr int SDL_CODE_RESOLUTION_CHANGED = 10;
constexpr int SDL_CODE_CONNECTION_INTERRUPTED = 112;
constexpr int ML_ERROR_GRACEFUL_TERMINATION = 0, ML_ERROR_HOST_RESTART = -5502;
struct SDL_Event
{
    int type = 0;
    struct
    {
        int event = 0;
    } window;
    struct
    {
        struct
        {
            int sym = 0;
        } keysym;
    } key;
    struct
    {
        int code = 0;
        void* data1 = nullptr;
        void* data2 = nullptr;
    } user;
    int cdevice = 0;
};
int64_t clockMs = 100;
int64_t fakeNow()
{
    return clockMs;
}
int64_t cancelAt = -1;
int cancelEventType = SDL_KEYDOWN;
bool interrupted = false;
bool workerIgnoresInterrupt = false;
int64_t firstInterrupt = -1;
bool workerSuccess = false;
bool terminalDuringWorker = false;
int workerDuration = 10;
std::vector<int64_t> attempts;
std::vector<int64_t> stops;
std::vector<bool> restartOptIns;
std::vector<uint32_t> attemptBudgets;
void LiPrepareConnectionAttempt(uint32_t remainingTimeoutMs)
{
    interrupted = false;
    attemptBudgets.push_back(remainingTimeoutMs);
}
void LiSetHostRestartRecoveryEnabled(bool enabled)
{
    restartOptIns.push_back(enabled);
}
Uint32 SDL_GetTicks()
{
    return static_cast<Uint32>(clockMs);
}
void SDL_Delay(int ms)
{
    clockMs += ms;
}
bool SDL_PollEvent(SDL_Event* event)
{
    if (cancelAt >= 0 && clockMs >= cancelAt) {
        cancelAt = -1;
        event->type = cancelEventType;
        event->key.keysym.sym = SDLK_ESCAPE;
        event->window.event = SDL_WINDOWEVENT_CLOSE;
        return true;
    }
    return false;
}
void SDL_LockMutex(int*) {}
void SDL_UnlockMutex(int*) {}
void LiStopConnection()
{
    stops.push_back(clockMs);
}
void LiInterruptConnection()
{
    interrupted = true;
    if (firstInterrupt < 0)
        firstInterrupt = clockMs;
}
void pushEventOrWarn(SDL_Event) {}
void SDL_LogWarn(int, const char*, ...) {}
int eligibilityLogs = 0;
void SDL_LogInfo(int, const char*, ...)
{
    ++eligibilityLogs;
}
void SDL_LogError(int, const char*, ...) {}
struct StreamQuality
{
    static QString resolutionName(QSize) { return "1920x1080"; }
};
struct Input
{
    void raiseAllKeys(bool = true) {}
    void notifyHostOfConnectedGamepads() {}
    void handleControllerDeviceEvent(int*, bool) {}
};
void stopControllerRumbleAtConnectionBoundary(Input*) {}
struct Haptics
{
    void setControllerTarget(int) {}
    void reset() {}
};
struct Clipboard
{
    void updateHostContext() {}
};
struct Toast
{
    void beginEventProcessing() {}
};

class Session
{
public:
    static Session* s_ActiveSession;
    enum class ReconnectReason
    {
        ConnectionLost,
        StreamQualityChange
    };
    int* m_DecoderLock = nullptr;
    int* m_VideoDecoder = nullptr;
    Input* m_InputHandler = nullptr;
    Haptics* m_DualSenseHapticsRenderer = nullptr;
    Clipboard* m_ClipboardHelper = nullptr;
    Toast* m_Toast = nullptr;
    void* m_Window = nullptr;
    std::atomic<bool> m_ShouldExit{ false };
    std::atomic<bool> m_ConnectionCancel{ false };
    std::atomic<bool> m_ReconnectInProgress{ false };
    std::atomic<bool> m_ConnectionInterrupted{ false };
    std::atomic<bool> m_ExplicitRestartRecovery{ true };
    std::atomic<bool> m_RestartRecoveryEligible{ false };
    std::atomic<bool> m_RecoveryFrameReceived{ false };
    bool m_RecoveryShowToast = false;
    bool m_RestartEligibilityLogged = false;
    bool m_AsyncConnectionSuccess = false;
    bool m_SuppressConnectionErrorDialog = false;
    bool m_HasReceivedVideo = false;
    bool m_UnexpectedTermination = true;
    bool m_PendingMicToggle = false;
    std::atomic<int> m_LastTerminationErrorCode{ ML_ERROR_HOST_RESTART };
    int displayedError = 999;
    int outcomeCount = 0;
    struct
    {
        int width = 1920, height = 1080, fps = 60;
    } m_StreamConfig;
    RecoveryBudget m_RecoveryBudget;
    std::vector<QString> toasts;
    bool tryReconnect(ReconnectReason reason = ReconnectReason::ConnectionLost);
    void processRecoveryStatus();
    bool prepareConnectionAttemptControl();
    bool interruptConnectionAttemptIfStopped();
    void logRestartRecoveryIneligibility();
    void exec();
    void displayTerminationError(int code)
    {
        displayedError = code;
        ++outcomeCount;
    }
    // HOST_FRAME_MARKER
    void teardownUsbTunnel() {}
    void stopSunshineAbr() {}
    void updateDualSenseHapticsControllerTarget() {}
    void processQtOverlayEvents() {}
    void handleSdlUserEvent(decltype(SDL_Event::user)) {}
    void handleHostResolutionReport(int, int) {}
    void scheduleWindowResolutionMatch() {}
    QString tr(const char* text) { return QString::fromUtf8(text); }
    void showStreamingToast(const QString& text, int) { toasts.push_back(text); }
};
Session* Session::s_ActiveSession = nullptr;

class AsyncConnectionStartThread
{
public:
    explicit AsyncConnectionStartThread(Session* session) : m_Session(session) {}
    void start()
    {
        m_Start = clockMs;
        interrupted = false;
        attempts.push_back(clockMs);
    }
    bool isRunning()
    {
        if ((!interrupted || workerIgnoresInterrupt) && clockMs - m_Start < workerDuration)
            return true;
        m_Session->m_AsyncConnectionSuccess = workerSuccess && !interrupted;
        if (terminalDuringWorker) {
            m_Session->m_ConnectionCancel = true;
            m_Session->m_LastTerminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
        }
        return false;
    }
    void wait() {}

private:
    Session* m_Session;
    int64_t m_Start = 0;
};

// RETRY_WORKER
// RECOVERY_STATUS
// ATTEMPT_CONTROL
// ATTEMPT_INTERRUPT
// ELIGIBILITY_LOG
// INITIAL_RECOVERY

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    auto reset = [] {
        clockMs = 100;
        cancelAt = -1;
        cancelEventType = SDL_KEYDOWN;
        workerSuccess = false;
        terminalDuringWorker = false;
        workerDuration = 10;
        workerIgnoresInterrupt = false;
        firstInterrupt = -1;
        attempts.clear();
        stops.clear();
        restartOptIns.clear();
        attemptBudgets.clear();
        eligibilityLogs = 0;
    };
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        workerDuration = 120000;
        check(!session.tryReconnect(), "hung attempt cannot count as recovered");
        check(clockMs <= 60110, "120-second worker must be interrupted at shared 60s deadline");
    }
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        workerSuccess = true;
        check(session.tryReconnect(), "successful connection may re-enter the stream loop");
        check(session.m_RecoveryBudget.active(), "connection success must not end the outage");
        bool falseHealthyToast = false;
        for (const auto& text : session.toasts)
            falseHealthyToast |= text == "Reconnected";
        check(!falseHealthyToast, "success without recovered frame must not claim Reconnected");
        clockMs = 50100;
        workerSuccess = false;
        session.tryReconnect();
        check(clockMs <= 60110, "repeated restart must retain original deadline");
    }
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        session.tryReconnect();
        check(attempts.size() >= 5, "offline host must retry with bounded backoff");
        if (attempts.size() >= 5) {
            check(attempts[1] - attempts[0] == 1010, "first retry backoff must be 1s");
            check(attempts[2] - attempts[1] == 2010, "second retry backoff must be 2s");
            check(attempts[3] - attempts[2] == 4010, "third retry backoff must be 4s");
            check(attempts[4] - attempts[3] == 8010, "fourth retry backoff must be 8s");
        }
    }
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        workerDuration = 120000;
        cancelAt = 130;
        check(!session.tryReconnect(), "Esc must cancel a pending connection attempt");
        check(session.m_ShouldExit, "Esc cancellation must request session exit");
        check(attempts.size() == 1, "cancel must prevent further automatic launches");
        check(clockMs < 200, "cancel must interrupt worker promptly");
    }
    for (int eventType : { SDL_QUIT, SDL_WINDOWEVENT, SDL_KEYDOWN }) {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        cancelEventType = eventType;
        cancelAt = 200;
        check(!session.tryReconnect() && session.m_ShouldExit,
              "close, quit and Esc during backoff must stop recovery");
        check(attempts.size() == 1, "backoff cancellation must prevent another launch");
    }
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        workerSuccess = true;
        terminalDuringWorker = true;
        check(!session.tryReconnect(), "delivered graceful stop wins connection-success race");
        check(attempts.size() == 1, "graceful stop must prevent another retry");
    }
    {
        reset();
        Session session;
        session.m_RecoveryBudget.begin(clockMs);
        workerSuccess = true;
        cancelAt = clockMs + workerDuration;
        check(!session.tryReconnect() && session.m_ShouldExit,
              "queued Esc at worker completion must win connection success");
        check(attempts.size() == 1, "completion-boundary cancel must not retry");
    }
    {
        reset();
        Session session;
        session.m_ConnectionCancel = true;
        check(!session.tryReconnect(), "pre-cancelled worker must fail without a launch");
        check(attempts.empty(), "pre-cancel must prevent the first automatic launch");
    }
    {
        reset();
        Session session;
        session.m_ConnectionInterrupted = true;
        session.m_RecoveryBudget.begin(clockMs);
        workerSuccess = true;
        session.exec();
        check(session.m_AsyncConnectionSuccess && attempts.size() == 1,
              "initial setup restart must recover before exec schedules failure cleanup");
        check(session.m_RecoveryBudget.active(),
              "initial setup recovery must await its first real frame on the same deadline");
    }
    {
        reset();
        Session session;
        session.m_ConnectionInterrupted = true;
        session.m_RecoveryBudget.begin(clockMs);
        workerDuration = 120000;
        session.exec();
        check(!session.m_AsyncConnectionSuccess && clockMs <= 60110 &&
                  session.displayedError == ML_ERROR_HOST_RESTART,
              "initial setup restart exhaustion must report failure within its original budget");
    }
    {
        reset();
        Session session;
        session.m_ConnectionInterrupted = false;
        session.exec();
        check(attempts.empty(), "bare setup failure must not bypass the received-video gate");
    }
    {
        reset();
        Session session;
        session.m_ConnectionInterrupted = true;
        session.m_RecoveryBudget.begin(clockMs);
        workerIgnoresInterrupt = true;
        workerDuration = 75000;
        session.exec();
        check(firstInterrupt == 60100, "deadline must interrupt even an uncooperative library");
        check(attempts.size() == 1, "no attempt may start after deadline while library unwinds");
        check(session.outcomeCount == 1 && session.displayedError == ML_ERROR_HOST_RESTART,
              "library returning after expiry must report exactly one recovery failure");
    }
    {
        reset();
        Session session;
        session.m_ConnectionInterrupted = true;
        session.m_RecoveryBudget.begin(clockMs);
        workerIgnoresInterrupt = true;
        workerDuration = 75000;
        cancelAt = 130;
        session.exec();
        check(firstInterrupt == 130 && attempts.size() == 1,
              "cancel must signal once admitted and never start another attempt");
        check(session.outcomeCount == 0 && session.m_ShouldExit,
              "cancelled library return must exit without a failure popup");
    }
    {
        reset();
        Session session;
        Session::s_ActiveSession = &session;
        session.m_RecoveryBudget.begin(clockMs);
        Session::markVideoReceived();
        check(session.m_RecoveryBudget.active(),
              "old connection's straggler frame must not recover");
        session.m_RecoveryBudget.awaitRecoveredFrame();
        clockMs = 60100;
        Session::markVideoReceived();
        check(session.m_RecoveryBudget.active(), "a frame after expiry must not renew the budget");
        session.processRecoveryStatus();
        check(session.m_ShouldExit && interrupted, "stream loop must interrupt at frame deadline");
        check(session.displayedError == ML_ERROR_HOST_RESTART,
              "no-frame outage expiry must report restart-recovery failure");
    }
    {
        reset();
        Session session;
        Session::s_ActiveSession = &session;
        session.m_RecoveryShowToast = true;
        session.m_RecoveryBudget.begin(clockMs);
        session.m_RecoveryBudget.awaitRecoveredFrame();
        clockMs = 60099;
        Session::markVideoReceived();
        check(!session.m_RecoveryBudget.active(), "first real recovered frame ends the outage");
        session.processRecoveryStatus();
        check(session.toasts.size() == 1 && session.toasts[0] == "Reconnected",
              "only first recovered host frame may emit Reconnected");
        session.processRecoveryStatus();
        check(session.toasts.size() == 1, "recovered notification must be emitted once");
    }
    {
        reset();
        Session session;
        Session::s_ActiveSession = &session;
        session.m_RecoveryBudget.begin(clockMs);
        session.m_RecoveryBudget.awaitRecoveredFrame();
        session.m_ConnectionCancel = true;
        Session::markVideoReceived();
        check(session.m_RecoveryBudget.active(), "cancel wins recovered-frame race");
        session.processRecoveryStatus();
        check(session.toasts.empty(), "cancelled recovery must not claim success");
    }
    check(restartRecoveryAppEligible("Desktop", false, 7, true), "Desktop must qualify");
    for (const char* name :
         { "Desktop Explorer", "desktop", "\xe6\xa1\x8c\xe9\x9d\xa2", "Renamed Desktop", "Game" }) {
        check(!restartRecoveryAppEligible(name, false, 7, true), "other app names fail closed");
    }
    check(!restartRecoveryAppEligible("Desktop", true, 7, true), "collector must fail closed");
    check(!restartRecoveryAppEligible("Desktop", false, 0, true), "zero ID must fail closed");
    check(!restartRecoveryAppEligible("Desktop", false, -1, true), "negative ID must fail closed");
    check(!restartRecoveryAppEligible("Desktop", false, 7, false),
          "unpaired/unauthenticated launch must fail closed");
    {
        reset();
        Session session;
        session.logRestartRecoveryIneligibility();
        session.logRestartRecoveryIneligibility();
        check(eligibilityLogs == 1, "ineligibility reason must be logged once per Session");
        check(session.prepareConnectionAttemptControl() &&
                  restartOptIns == std::vector<bool>{ false },
              "ineligible connection must never advertise restart opt-in");
        check(attemptBudgets == std::vector<uint32_t>{ UINT32_MAX },
              "ordinary connection must prepare with no additional library deadline");
        session.m_RestartRecoveryEligible = true;
        check(session.prepareConnectionAttemptControl() && restartOptIns.back(),
              "authenticated original Desktop must advertise the provisional opt-in");
        session.m_RecoveryBudget.begin(clockMs);
        clockMs += 59000;
        check(session.prepareConnectionAttemptControl() && !attemptBudgets.empty() &&
                  attemptBudgets.back() == 1000,
              "library must inherit the remaining outage time, not a fresh sixty seconds");
        session.m_ConnectionCancel = true;
        check(!session.prepareConnectionAttemptControl() && interrupted,
              "cancel latched before prepare must be reasserted after library reset");
        session.m_ConnectionCancel = false;
        clockMs += 1000;
        const auto calls = restartOptIns.size();
        check(!session.prepareConnectionAttemptControl() && restartOptIns.size() == calls,
              "expired deadline must prevent RTSP connection entry");
    }
    Session::s_ActiveSession = nullptr;
    if (failures)
        return 1;
    std::cout << "retry worker deadline passed\n";
    return 0;
}
