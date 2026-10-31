#include <atomic>
#include <cstdint>
#include <iostream>
#include <vector>
#include "recoverybudget.h"

constexpr int ML_ERROR_GRACEFUL_TERMINATION = 0;
constexpr int ML_ERROR_PROTECTED_CONTENT = -103;
constexpr int ML_ERROR_FRAME_CONVERSION = -104;
constexpr int ML_ERROR_HOST_RESTART = -5502; // Owner's provisional common-c allocation.
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
constexpr int SDL_QUIT = 1;
constexpr int SDL_USEREVENT = 2, SDL_CODE_CONNECTION_INTERRUPTED = 112;
uint32_t ticks = 100;
struct SDL_Event
{
    int type;
    struct
    {
        uint32_t timestamp;
    } quit;
    struct
    {
        int code;
    } user;
};
std::vector<SDL_Event> events;
int connectivityTests = 0;
unsigned int LiGetPortFlagsFromTerminationErrorCode(int)
{
    return 0;
}
int LiTestClientConnectivity(const char*, int, unsigned int)
{
    ++connectivityTests;
    return 0;
}
constexpr const char* CONN_TEST_SERVER = "unused";
void SDL_LogWarn(int, const char*, ...) {}
void SDL_LogError(int, const char*, ...) {}
uint32_t SDL_GetTicks()
{
    return ticks;
}
void pushEventOrWarn(SDL_Event event)
{
    events.push_back(event);
}

class Session
{
public:
    static Session* s_ActiveSession;
    std::atomic<bool> m_HasReceivedVideo{ false };
    bool m_ShouldExit = false;
    std::atomic<bool> m_ConnectionInterrupted{ false };
    std::atomic<bool> m_ReconnectInProgress{ false };
    std::atomic<bool> m_RestartRecoveryEligible{ true };
    std::atomic<bool> m_ConnectionCancel{ false };
    std::atomic<bool> m_ExplicitRestartRecovery{ false };
    std::atomic<bool> m_RecoveryFrameReceived{ false };
    RecoveryBudget m_RecoveryBudget;
    int m_LastTerminationErrorCode = 0;
    int m_PortTestResults = 0;
    int displayedErrors = 0;
    void displayTerminationError(int) { ++displayedErrors; }
    static void clConnectionTerminated(int errorCode);
};
Session* Session::s_ActiveSession = nullptr;

// TERMINATION_CALLBACK

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    auto reset = [&] {
        events.clear();
        connectivityTests = 0;
    };
    {
        Session session;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        check(session.m_ConnectionInterrupted,
              "authenticated Desktop restart before first host frame must enter recovery");
        check(session.displayedErrors == 0, "eligible restart must not show terminal popup");
        check(connectivityTests == 0, "restart deadline must not wait for unrelated port tests");
        check(events.size() == 1, "first restart must wake the owning event loop exactly once");
    }
    for (int terminal :
         { ML_ERROR_GRACEFUL_TERMINATION, ML_ERROR_PROTECTED_CONTENT, ML_ERROR_FRAME_CONVERSION }) {
        Session session;
        session.m_HasReceivedVideo = true;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(terminal);
        check(!session.m_ConnectionInterrupted, "delivered terminal stop must not reconnect");
    }
    for (int terminal :
         { ML_ERROR_GRACEFUL_TERMINATION, ML_ERROR_PROTECTED_CONTENT, ML_ERROR_FRAME_CONVERSION }) {
        Session session;
        session.m_HasReceivedVideo = true;
        session.m_ReconnectInProgress = true;
        session.m_RecoveryBudget.begin(RecoveryBudget::nowMs());
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(terminal);
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        check(session.m_ConnectionCancel && !session.m_ConnectionInterrupted,
              "delivered stop during recovery wins a subsequent restart notice");
        check(session.m_LastTerminationErrorCode == terminal,
              "subsequent restart must not replace the delivered terminal reason");
        check(events.empty() && session.displayedErrors == 0,
              "worker owns the sole terminal outcome without duplicate events or popups");
    }
    {
        Session session;
        session.m_HasReceivedVideo = true;
        session.m_ReconnectInProgress = true;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        check(events.empty(), "repeated restart during worker must not look like user cancel");
        check(session.displayedErrors == 0, "repeated restart must not show terminal popup");
    }
    {
        Session session;
        session.m_RestartRecoveryEligible = false;
        session.m_HasReceivedVideo = true;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        check(!session.m_ConnectionInterrupted,
              "bare, game or unauthenticated session must not recover explicit restart");
    }
    {
        Session session;
        session.m_ShouldExit = true;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(ML_ERROR_HOST_RESTART);
        check(!session.m_ConnectionInterrupted, "user stop wins a restart callback race");
    }
    {
        Session session;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(-100);
        check(!session.m_ConnectionInterrupted,
              "bare generic loss must retain received-video gate");
    }
    {
        Session session;
        session.m_HasReceivedVideo = true;
        Session::s_ActiveSession = &session;
        reset();
        Session::clConnectionTerminated(-100);
        check(session.m_ConnectionInterrupted,
              "pulled host frame still permits generic-loss retry");
    }
    Session::s_ActiveSession = nullptr;
    if (failures)
        return 1;
    std::cout << "termination recovery passed\n";
    return 0;
}
