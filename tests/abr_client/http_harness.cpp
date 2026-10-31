#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <memory>
#include <stdexcept>

// EXCEPTIONS

class IdentityManager
{
public:
    static IdentityManager* get()
    {
        static IdentityManager identity;
        return &identity;
    }
    QSslConfiguration getSslConfig() { return QSslConfiguration::defaultConfiguration(); }
};

// The wire tests use HTTP only. An HTTPS response without a paired certificate
// must still be rejected before any status/body is interpreted.
namespace PairedCertificate {
QMetaObject::Connection enforce(QNetworkAccessManager*, QObject*, const QSslCertificate&)
{
    return {};
}
bool matches(const QSslCertificate&, const QSslCertificate&)
{
    return false;
}
}

class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest& request, int status, NetworkError error, QByteArray body,
              bool pending, QObject* parent)
        : QNetworkReply(parent), m_Body(body)
    {
        setRequest(request);
        setUrl(request.url());
        if (status) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        }
        if (error != NoError) {
            setError(error, QStringLiteral("Qt generic error"));
        }
        open(QIODevice::ReadOnly);
        if (!pending) {
            QTimer::singleShot(0, this, [this] {
                setFinished(true);
                emit finished();
            });
        }
    }
    void abort() override
    {
        setError(OperationCanceledError, QStringLiteral("Aborted"));
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override
    {
        return m_Body.size() - m_Offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 count = qMin(maxSize, m_Body.size() - m_Offset);
        if (!count)
            return -1;
        memcpy(data, m_Body.constData() + m_Offset, count);
        m_Offset += count;
        return count;
    }

private:
    QByteArray m_Body;
    qint64 m_Offset = 0;
};

class FakeManager : public QNetworkAccessManager
{
public:
    int status = 400;
    QNetworkReply::NetworkError error = QNetworkReply::ProtocolInvalidOperationError;
    QByteArray body =
        R"({"error_code":"no_active_stream","error":"No active streaming session for this client"})";
    bool pending = false;
    QByteArray lastBody;
    QString lastPath;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request,
                                 QIODevice* data) override
    {
        lastBody = data ? data->readAll() : QByteArray();
        lastPath = request.url().path();
        return new FakeReply(request, status, error, body, pending, this);
    }
};

class NvHTTP : public QObject
{
public:
    enum NvLogLevel
    {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };
    QNetworkAccessManager* m_Nam;
    QSslCertificate m_ServerCert;
    std::shared_ptr<std::atomic_bool> m_RequestCancellation;
    QUrl m_BaseUrlHttps = QUrl("http://fixture.invalid:47984");
    explicit NvHTTP(QNetworkAccessManager* nam) : m_Nam(nam) {}
    void handleSslErrors(QNetworkReply*, const QList<QSslError>&) {}
    QNetworkReply* openJsonConnection(QUrl, QString, QJsonObject, bool, int, NvLogLevel);
    QJsonObject openJsonConnectionToObject(QUrl, QString, QJsonObject, bool, int, NvLogLevel);
    bool getAbrCapabilities(int* = nullptr, QJsonObject* = nullptr);
    QJsonObject configureAbr(bool, int, int, QString, int, QString = {});
    QJsonObject sendAbrFeedback(double, double, double, int, int, int, QString = {});
};

constexpr int FAST_FAIL_TIMEOUT_MS = 100;
// TRANSPORT

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    FakeManager manager;
    NvHTTP http(&manager);
    auto request = [&](QString scheme = QStringLiteral("http")) {
        return std::unique_ptr<QNetworkReply>(
            http.openJsonConnection(QUrl(scheme + QStringLiteral("://fixture.invalid:47984")),
                                    "api/abr", {}, true, 50, NvHTTP::NVLL_NONE));
    };
    try {
        for (auto entry : { std::pair<int, QNetworkReply::NetworkError>{
                                400, QNetworkReply::ProtocolInvalidOperationError },
                            { 401, QNetworkReply::AuthenticationRequiredError },
                            { 403, QNetworkReply::ContentAccessDenied },
                            { 404, QNetworkReply::ContentNotFoundError },
                            { 409, QNetworkReply::ContentConflictError },
                            { 503, QNetworkReply::ServiceUnavailableError } }) {
            manager.status = entry.first;
            manager.error = entry.second;
            try {
                request();
                throw std::runtime_error("HTTP failure was accepted");
            } catch (const GfeHttpResponseException& e) {
                require(e.getStatusCode() == entry.first, "HTTP status was lost");
                require(QString::fromUtf8(e.what()).contains("no_active_stream"),
                        "HTTP 400 JSON body was lost");
                require(e.getResponseBody() == manager.body, "Raw error body was lost");
                require(e.getErrorObject().value("error_code") == "no_active_stream",
                        "Structured error object was lost");
            } catch (const QtNetworkReplyException&) {
                throw std::runtime_error("HTTP 400/status JSON was discarded as Qt network error");
            }
        }
        try {
            request(QStringLiteral("https"));
            throw std::runtime_error("Unpaired HTTPS error body trusted");
        } catch (const QtNetworkReplyException& e) {
            require(e.getError() == QNetworkReply::SslHandshakeFailedError,
                    "HTTPS HTTP error bypassed certificate verification");
        }
        manager.status = 400;
        manager.error = QNetworkReply::ProtocolInvalidOperationError;
        manager.body = "<html>Bad request</html>";
        try {
            request();
            throw std::runtime_error("Non-JSON HTTP failure was accepted");
        } catch (const GfeHttpResponseException& e) {
            require(e.getResponseBody() == manager.body, "Malformed error body lost");
            require(e.getErrorObject().isEmpty(), "Malformed body invented structured error");
        }
        manager.status = 302;
        manager.error = QNetworkReply::NoError;
        try {
            request();
            throw std::runtime_error("Redirect accepted");
        } catch (const GfeHttpResponseException& e) {
            require(e.getStatusCode() == 302, "Redirect status lost");
        }
        manager.status = 200;
        manager.error = QNetworkReply::NoError;
        manager.body = R"({"success":true})";
        require(request()->readAll() == manager.body, "Success body consumed");
        http.configureAbr(true, 0, 1000, "balanced", 50);
        require(!QJsonDocument::fromJson(manager.lastBody).object().contains("sessionToken"),
                "Initial configure unexpectedly bound to a token");
        http.configureAbr(false, 0, 0, "balanced", 50, "old-generation-token");
        auto body = QJsonDocument::fromJson(manager.lastBody).object();
        require(body.value("enabled") == QJsonValue(false) &&
                    body.value("sessionToken") == "old-generation-token",
                "Production disable serialization omitted its token");
        http.sendAbrFeedback(1.5, 12, 60, 3, 1000, 50, "current-generation-token");
        body = QJsonDocument::fromJson(manager.lastBody).object();
        require(manager.lastPath == "/api/abr/feedback" &&
                    body.value("sessionToken") == "current-generation-token" &&
                    body.value("currentBitrate") == 1000 && body.value("packetLoss") == 1.5 &&
                    body.value("rttMs") == 12 && body.value("decodeFps") == 60 &&
                    body.value("droppedFrames") == 3,
                "Production feedback serialization changed its telemetry/token shape");
        manager.body =
            R"({"supported":true,"version":2,"features":["session_binding"],"hostMaxBitrate":5000})";
        QJsonObject capabilities;
        int maxBitrate = 0;
        require(http.getAbrCapabilities(&maxBitrate, &capabilities) && maxBitrate == 5000 &&
                    capabilities.value("features").toArray().contains("session_binding"),
                "Production capability negotiation lost version/features");
        manager.body = "[1,2]";
        try {
            http.openJsonConnectionToObject(QUrl("http://fixture.invalid:47984"), "api/abr", {},
                                            true, 50, NvHTTP::NVLL_NONE);
            throw std::runtime_error("Malformed success body was accepted");
        } catch (const GfeHttpResponseException& e) {
            require(e.getStatusCode() == 200, "Malformed JSON lost HTTP status");
            require(e.getResponseBody() == manager.body, "Malformed JSON lost raw body");
        }
        manager.body = R"({"success":true})";
        try {
            request(QStringLiteral("https"));
            throw std::runtime_error("Unpaired HTTPS success was accepted");
        } catch (const QtNetworkReplyException& e) {
            require(e.getError() == QNetworkReply::SslHandshakeFailedError,
                    "Certificate mismatch was reclassified");
        }
        manager.status = 0;
        for (auto error :
             { QNetworkReply::HostNotFoundError, QNetworkReply::SslHandshakeFailedError,
               QNetworkReply::RemoteHostClosedError }) {
            manager.error = error;
            try {
                request();
                throw std::runtime_error("Network failure was accepted");
            } catch (const QtNetworkReplyException& e) {
                require(e.getError() == error, "Network failure was reclassified");
            }
        }
        manager.pending = true;
        try {
            request();
            throw std::runtime_error("Timed-out reply was accepted");
        } catch (const QtNetworkReplyException& e) {
            require(e.getError() == QNetworkReply::TimeoutError, "Timeout was reclassified");
        }
        http.m_RequestCancellation = std::make_shared<std::atomic_bool>(true);
        try {
            request();
            throw std::runtime_error("Cancelled queued request was sent");
        } catch (const QtNetworkReplyException& e) {
            require(e.getError() == QNetworkReply::OperationCanceledError,
                    "Cancellation was reported as timeout");
        }
        http.m_RequestCancellation->store(false);
        QTimer::singleShot(1, [&] { http.m_RequestCancellation->store(true); });
        try {
            request();
            throw std::runtime_error("Cancelled in-flight request was accepted");
        } catch (const QtNetworkReplyException& e) {
            require(e.getError() == QNetworkReply::OperationCanceledError,
                    "In-flight cancellation lost");
        }
        qInfo("http-regressions passed");
        return 0;
    } catch (const std::exception& e) {
        qCritical("FAIL: %s", e.what());
        return 1;
    }
}
