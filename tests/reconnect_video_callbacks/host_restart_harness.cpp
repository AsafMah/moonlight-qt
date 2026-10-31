#include <QCoreApplication>
#include <QDebug>
#include <QNetworkReply>
#include <QReadWriteLock>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>
#include <iostream>
#include <stdexcept>
#include <utility>

constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
constexpr int FAST_FAIL_TIMEOUT_MS = 2000;
constexpr int REQUEST_TIMEOUT_MS = 5000;
void SDL_LogInfo(int, const char*, ...) {}
void SDL_LogWarn(int, const char*, ...) {}

class GfeHttpResponseException : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
    GfeHttpResponseException(int, const QString& message)
        : std::runtime_error(message.toStdString())
    {
    }
    QString toQString() const { return QString::fromUtf8(what()); }
};
class QtNetworkReplyException : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
    QString toQString() const { return QString::fromUtf8(what()); }
};

struct NvComputer
{
    QReadWriteLock lock;
    int currentGameId = 7;
    int serverCodecModeSupport = 1;
    QString appVersion = "1";
    QString gfeVersion = "1";
    bool isNvidiaServerSoftware = false;
};

// HOST_SNAPSHOT

HostConnectionInfoSnapshot captureHostConnectionInfoSnapshot(NvComputer* computer)
{
    QReadLocker lock(&computer->lock);
    HostConnectionInfoSnapshot snapshot;
    snapshot.currentGameId = computer->currentGameId;
    snapshot.serverCodecModeSupport = computer->serverCodecModeSupport;
    snapshot.appVersion = computer->appVersion;
    snapshot.gfeVersion = computer->gfeVersion;
    return snapshot;
}

struct Host
{
    int currentGameId = 0;
    bool failRefresh = false;
    bool failLaunch = false;
    bool resetBeforeResume = false;
    bool missingCert = false;
    bool missingPort = false;
    bool rejectedAuthentication = false;
    QString invalidState;
    QVector<QString> calls;
    QVector<int> appIds;
    QVector<int> failures;
} host;

class NvHTTP
{
public:
    enum NvLogLevel
    {
        NVLL_NONE
    };
    explicit NvHTTP(NvComputer*) {}
    struct Cert
    {
        bool isNull() const { return host.missingCert; }
    } m_ServerCert;
    QUrl m_BaseUrlHttps{ "https://paired-host:47984" };
    int httpsPort() const { return host.missingPort ? 0 : 47984; }
    static QString getXmlString(QString xml, QString tagName);
    static int getCurrentGame(QString serverInfo);
    static void verifyResponseStatus(const QString&)
    {
        if (host.rejectedAuthentication)
            throw GfeHttpResponseException("401 unpaired host");
    }
    QString response()
    {
        const QString state =
            host.invalidState.isEmpty()
                ? (host.currentGameId ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE")
                : host.invalidState;
        return QString("<root status_code=\"200\"><state>%1</state><currentgame>%2</currentgame>"
                       "<appversion>1</appversion><GfeVersion>1</GfeVersion>"
                       "<ServerCodecModeSupport>1</ServerCodecModeSupport></root>")
            .arg(state)
            .arg(host.currentGameId);
    }
    QString getServerInfo(NvLogLevel, bool)
    {
        host.calls.append("post");
        return response();
    }
    QString getAuthenticatedServerInfo(NvLogLevel, bool);
    QString openConnectionToString(QUrl url, QString command, QString, int timeout, NvLogLevel)
    {
        if (url.scheme() != "https" || command != "serverinfo" || timeout != 2000)
            throw std::runtime_error("retry refresh must use fast-fail HTTPS serverinfo");
        host.calls.append("refresh");
        if (host.failRefresh)
            throw QtNetworkReplyException("authentication/transport failure");
        return response();
    }
    template <typename... Args> void startApp(const QString& verb, bool, int appId, Args&&...)
    {
        host.calls.append(verb);
        host.appIds.append(appId);
        if (verb == "resume" && host.resetBeforeResume) {
            host.currentGameId = 0;
            host.resetBeforeResume = false;
        }
        if (verb == "resume" && host.currentGameId == 0) {
            host.failures.append(503);
            throw GfeHttpResponseException("503 NO_APP_TO_RESUME");
        }
        if (host.failLaunch) {
            host.failures.append(503);
            throw GfeHttpResponseException("503 unrelated launch failure");
        }
        host.currentGameId = appId;
    }
};

// AUTHENTICATED_METHODS

// The production metadata helper uses the transport's XML helpers.
// HOST_UPDATE

struct RemoteStreamConfig
{
    template <typename... Args> explicit RemoteStreamConfig(Args&&...) {}
};

class Session
{
public:
    NvComputer computer;
    NvComputer* m_Computer = &computer;
    struct App
    {
        int id = 7;
    } m_App;
    struct Preferences
    {
        int remoteResolutionWidth = 0;
        int remoteResolutionHeight = 0;
        int remoteFpsRate = 0;
        bool playAudioOnHost = false;
        bool multiController = true;
        int screenCombinationMode = -1;
    } preferences;
    Preferences* m_Preferences = &preferences;
    struct Input
    {
        int getAttachedGamepadMask() { return 0; }
    } input;
    Input* m_InputHandler = &input;
    int m_StreamConfig = 0;
    int m_LaunchUseVdd = 0;
    QString m_LaunchDisplayName;
    float m_LastClientSdrWhiteNits = 0;
    bool m_SuppressConnectionErrorDialog = true;
    QString tr(const char* text) { return QString::fromUtf8(text); }
    void displayLaunchError(const QString&) {}
    bool attempt()
    {
        bool remoteResolution = false, remoteFps = false, enableGameOptimizations = false;
        int originalStreamWidth = 1280, originalStreamHeight = 720;
        float maxBrightness = 0, minBrightness = 0, maxAverageBrightness = 0;
        float sdrWhiteBrightness = 0;
        QString rtspSessionUrl;
        // ATTEMPT_PRELUDE
        // ATTEMPT_HTTP
        return true;
    }
};

bool require(bool condition, const char* description)
{
    if (!condition)
        std::cerr << "FAIL: " << description << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    bool ok = true;
    {
        Session session;
        host = {};
        // Cached busy app; Core has respawned FREE and would reject /resume with 503.
        ok &= require(session.attempt(), "cached busy -> FREE must recover without stale resume");
        ok &= require(host.calls == QVector<QString>{ "refresh", "launch", "post" } &&
                          host.appIds == QVector<int>{ 7 },
                      "fresh launch must use original app ID after authenticated refresh");
    }
    {
        Session session;
        host = {};
        host.currentGameId = 7;
        host.resetBeforeResume = true;
        ok &= require(!session.attempt() && host.failures == QVector<int>{ 503 },
                      "Core reset after refresh must produce the concrete resume 503");
        ok &= require(session.attempt(), "next attempt must refresh FREE state and launch");
        ok &= require(host.calls ==
                          QVector<QString>{ "refresh", "resume", "refresh", "launch", "post" },
                      "resume 503 must not launch blindly in the same attempt");
    }
    {
        Session session;
        host = {};
        host.currentGameId = 99;
        ok &= require(!session.attempt() && host.appIds.isEmpty(),
                      "a different busy app must never be resumed or taken over");
    }
    {
        Session session;
        host = {};
        host.failRefresh = true;
        ok &= require(!session.attempt() && host.appIds.isEmpty(),
                      "failed authenticated refresh must never fall back to cached state");
    }
    for (int failure = 0; failure < 4; ++failure) {
        Session session;
        host = {};
        host.missingCert = failure == 0;
        host.missingPort = failure == 1;
        host.invalidState = failure == 2 ? "UNKNOWN_STATE" : "";
        host.rejectedAuthentication = failure == 3;
        ok &= require(!session.attempt() && host.appIds.isEmpty(),
                      "missing pairing/HTTPS port, invalid state or HTTPS 401 must never launch");
    }
    {
        Session session;
        host = {};
        host.invalidState = "SUNSHINE_SERVER_BUSY";
        ok &= require(!session.attempt() && host.appIds.isEmpty(),
                      "busy state with no valid app must not be treated as free");
    }
    {
        Session session;
        host = {};
        host.currentGameId = 7;
        host.failLaunch = true;
        ok &=
            require(!session.attempt() && !session.attempt(), "unrelated 503 must remain failure");
        ok &= require(host.calls == QVector<QString>{ "refresh", "resume", "refresh", "resume" },
                      "unrelated 503 must not cause blind launch");
    }
    {
        Session session;
        host = {};
        host.currentGameId = 7;
        ok &= require(session.attempt() &&
                          host.calls == QVector<QString>{ "refresh", "resume", "post" },
                      "the same running app must resume");
    }
    {
        Session session;
        host = {};
        host.currentGameId = 99;
        host.invalidState = "SUNSHINE_SERVER_FREE";
        ok &= require(session.attempt() &&
                          host.calls == QVector<QString>{ "refresh", "launch", "post" },
                      "FREE state must disregard a stale currentgame field");
    }
    {
        Session session;
        host = {};
        session.computer.currentGameId = 0;
        session.m_SuppressConnectionErrorDialog = false;
        ok &= require(session.attempt() && host.calls == QVector<QString>{ "launch", "post" },
                      "ordinary initial launch must not add retry preflight");
    }
    if (ok)
        std::cout << "host restart recovery passed\n";
    return ok ? 0 : 1;
}
