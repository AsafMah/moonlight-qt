#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <vector>

class QString
{
public:
    QString() = default;
    QString(const char* text) : m_Value(text) {}
    QString(std::string text) : m_Value(std::move(text)) {}

    QString arg(const QString& replacement) const
    {
        std::string value = m_Value;
        const size_t marker = value.find("%1");
        if (marker != std::string::npos) {
            value.replace(marker, 2, replacement.m_Value);
        }
        return value;
    }

    bool isEmpty() const { return m_Value.empty(); }
    void clear() { m_Value.clear(); }
    const std::string& value() const { return m_Value; }

private:
    std::string m_Value;
};

static QString tr(const char* text) { return QString(text); }

namespace OverlayMenuPanel {
struct StreamConfigurationRequest
{
    int codec = 0;
    int hdrMode = 0;
    int audioConfig = 0;
};
}

namespace StreamQuality {
struct LiveResolutionTransaction
{
    bool active = false;
    bool isActive() const { return active; }
};
}

static constexpr int SDL_USEREVENT = 24;
static constexpr int SDL_CODE_APPLY_STREAM_CONFIGURATION = 114;
struct SDL_UserEvent
{
    int code = 0;
};
struct SDL_Event
{
    int type = 0;
    SDL_UserEvent user;
};

enum class ReconnectReason
{
    StreamQualityChange
};

#define emit

struct Session
{
    struct StreamConfiguration
    {
        int codec = 0;
        int hdrMode = 0;
        int audioConfig = 0;
    };
    struct PreparedStreamConfiguration
    {
        StreamConfiguration streamConfig;
        int videoFormats = 0;
        int videoCallbacks = 0;
        int audioCallbacks = 0;
        bool pyrowaveRequested = false;
        QString resultMessage;
    };

    static Session* s_ActiveSession;
    std::vector<SDL_Event> queuedEvents;
    std::optional<OverlayMenuPanel::StreamConfigurationRequest> m_PendingStreamConfiguration;
    std::optional<PreparedStreamConfiguration> m_PreparedStreamConfiguration;
    OverlayMenuPanel::StreamConfigurationRequest m_RequestedStreamConfiguration;
    StreamConfiguration m_StreamConfig;
    StreamQuality::LiveResolutionTransaction m_LiveResolutionTransaction;
    bool m_PendingStreamQuality = false;
    bool m_SuppressConnectionErrorDialog = false;
    bool m_ShouldExit = false;
    int m_SupportedVideoFormats = 0;
    int m_VideoCallbacks = 0;
    int m_AudioCallbacks = 0;
    bool m_PyrowaveRequested = false;
    QString m_StreamConfigurationResultMessage;
    bool preparationSucceeds = true;
    bool reconnectSucceeds = true;
    QString preparationFailure = "unsupported";
    int preparationCalls = 0;
    int reconnectCalls = 0;
    int stateUpdates = 0;
    int errorDialogs = 0;
    ReconnectReason reconnectReason = ReconnectReason::StreamQualityChange;
    OverlayMenuPanel::StreamConfigurationRequest preparedRequest;
    OverlayMenuPanel::StreamConfigurationRequest visiblePending;

    void showStreamingToast(QString, int = 5000) {}
    void updateStreamConfigurationMenuState()
    {
        ++stateUpdates;
        if (m_PendingStreamConfiguration) {
            visiblePending = *m_PendingStreamConfiguration;
        }
    }
    void updateStreamQualityMenuState() {}
    void updateConnectionMenuInfo() {}
    void displayLaunchError(QString) { ++errorDialogs; }
    bool prepareStreamConfiguration(
        const OverlayMenuPanel::StreamConfigurationRequest& request,
        PreparedStreamConfiguration& prepared, QString& failureReason)
    {
        ++preparationCalls;
        preparedRequest = request;
        if (!preparationSucceeds) {
            failureReason = preparationFailure;
            return false;
        }
        prepared.streamConfig = { request.codec, request.hdrMode, request.audioConfig };
        prepared.videoFormats = request.codec;
        prepared.videoCallbacks = request.hdrMode;
        prepared.audioCallbacks = request.audioConfig;
        return true;
    }
    void applyPreparedStreamConfiguration();
    bool tryReconnect(ReconnectReason reason)
    {
        ++reconnectCalls;
        reconnectReason = reason;
        if (!reconnectSucceeds) {
            return false;
        }
        applyPreparedStreamConfiguration();
        return true;
    }
    bool applyPendingStreamConfigurationChange();
    void requestStreamConfigurationChange(
        const OverlayMenuPanel::StreamConfigurationRequest& request);
};

Session* Session::s_ActiveSession = nullptr;

static void pushEventOrWarn(SDL_Event& event)
{
    if (Session::s_ActiveSession != nullptr) {
        Session::s_ActiveSession->queuedEvents.push_back(event);
    }
}

// METHODS

static bool require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

static bool dispatchOne(Session& session)
{
    if (session.queuedEvents.empty()) {
        return false;
    }
    const SDL_Event event = session.queuedEvents.front();
    session.queuedEvents.erase(session.queuedEvents.begin());
    if (event.type == SDL_USEREVENT &&
        event.user.code == SDL_CODE_APPLY_STREAM_CONFIGURATION) {
        return session.applyPendingStreamConfigurationChange();
    }
    return false;
}

int main()
{
    bool ok = true;
    Session session;
    Session::s_ActiveSession = &session;
    const OverlayMenuPanel::StreamConfigurationRequest request{ 4, 2, 3 };
    session.requestStreamConfigurationChange(request);
    ok &= require(session.queuedEvents.size() == 1 &&
                      session.queuedEvents.front().user.code == SDL_CODE_APPLY_STREAM_CONFIGURATION,
                  "configuration request queues the Session apply event");
    ok &= require(session.m_PendingStreamConfiguration.has_value(),
                  "pending request remains in Session until handled");

    session.requestStreamConfigurationChange({ 1, 0, 0 });
    ok &= require(session.queuedEvents.size() == 1 &&
                      session.m_PendingStreamConfiguration->codec == request.codec,
                  "a second request cannot replace the in-flight request");
    ok &= require(dispatchOne(session), "queued configuration event is handled");
    ok &= require(session.preparationCalls == 1 && session.reconnectCalls == 1 &&
                      session.reconnectReason == ReconnectReason::StreamQualityChange,
                  "event prepares once and uses the existing settings reconnect reason");
    ok &= require(session.m_StreamConfig.codec == request.codec &&
                      session.m_StreamConfig.hdrMode == request.hdrMode &&
                      session.m_StreamConfig.audioConfig == request.audioConfig &&
                      session.m_SupportedVideoFormats == request.codec &&
                      session.m_VideoCallbacks == request.hdrMode &&
                      session.m_AudioCallbacks == request.audioConfig,
                  "prepared values are consumed by the reconnect boundary");
    ok &= require(!session.m_PendingStreamConfiguration &&
                      session.m_RequestedStreamConfiguration.codec == request.codec,
                  "success clears pending and commits requested values");

    Session rejected;
    Session::s_ActiveSession = &rejected;
    rejected.preparationSucceeds = false;
    rejected.requestStreamConfigurationChange(request);
    ok &= require(dispatchOne(rejected), "preflight rejection is handled without session exit");
    ok &= require(rejected.reconnectCalls == 0 && rejected.m_StreamConfig.codec == 0 &&
                      rejected.m_RequestedStreamConfiguration.codec == 0 &&
                      !rejected.m_PendingStreamConfiguration &&
                      rejected.m_StreamConfigurationResultMessage.value().find("Not applied") == 0,
                  "preflight rejection preserves active/requested values and reports failure");

    Session reconnectFailed;
    Session::s_ActiveSession = &reconnectFailed;
    reconnectFailed.reconnectSucceeds = false;
    reconnectFailed.requestStreamConfigurationChange(request);
    ok &= require(!dispatchOne(reconnectFailed), "failed reconnect ends the apply path");
    ok &= require(reconnectFailed.m_StreamConfig.codec == 0 &&
                      reconnectFailed.m_RequestedStreamConfiguration.codec == 0 &&
                      reconnectFailed.errorDialogs == 1 &&
                      reconnectFailed.m_StreamConfigurationResultMessage.value().find(
                          "Reconnect failed") == 0,
                  "reconnect failure preserves accepted values and reports explicit error");
    Session::s_ActiveSession = nullptr;
    return ok ? 0 : 1;
}
