#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

class QtNetworkReplyException : public std::runtime_error
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError code, const QString& text)
        : std::runtime_error(text.toStdString()), error(code)
    {
    }
    QNetworkReply::NetworkError error;
};

class GfeHttpResponseException : public std::runtime_error
{
public:
    GfeHttpResponseException(int, const QString& text) : std::runtime_error(text.toStdString()) {}
};

struct IdentityManager
{
    static IdentityManager* get()
    {
        static IdentityManager instance;
        return &instance;
    }
    QString getUniqueId() { return "test"; }
    QSslConfiguration getSslConfig() { return {}; }
};

struct NvComputer
{
    static QString getPairname(const QString&) { return {}; }
};

namespace PairedCertificate {
QMetaObject::Connection enforce(QNetworkAccessManager*, QObject*, const QSslCertificate&)
{
    return {};
}
bool matches(const QSslCertificate&, const QSslCertificate&)
{
    return true;
}
}

class ScriptedReply : public QNetworkReply
{
public:
    explicit ScriptedReply(QObject* parent, int finishMs) : QNetworkReply(parent)
    {
        open(QIODevice::ReadOnly);
        if (finishMs >= 0) {
            QTimer::singleShot(finishMs, this, [this] {
                setFinished(true);
                emit finished();
            });
        }
    }
    void abort() override
    {
        setError(OperationCanceledError, "scripted abort");
        setFinished(true);
        emit finished();
    }

protected:
    qint64 readData(char*, qint64) override { return 0; }
};

class ScriptedNetwork : public QNetworkAccessManager
{
public:
    int requests = 0;
    int finishMs = -1;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override
    {
        ++requests;
        return new ScriptedReply(this, finishMs);
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
    explicit NvHTTP(ScriptedNetwork* nam) : m_Nam(nam) {}
    void setRequestControl(std::function<int()> remainingMs, const std::atomic<bool>* cancelled)
    {
        m_RequestRemainingMs = std::move(remainingMs);
        m_RequestCancelled = cancelled;
    }
    QNetworkReply* openConnection(QUrl baseUrl, QString command, QString arguments, int timeoutMs,
                                  NvLogLevel logLevel, qint64 maxResponseBytes = 0);
    void handleSslErrors(QNetworkReply*, const QList<QSslError>&) {}
    QNetworkAccessManager* m_Nam;
    QSslCertificate m_ServerCert;
    bool m_UseTrueUid = false;
    QString m_Uuid;
    std::function<int()> m_RequestRemainingMs;
    const std::atomic<bool>* m_RequestCancelled = nullptr;
};

// HTTP_TRANSPORT

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    auto request = [](NvHTTP& http) {
        try {
            std::unique_ptr<QNetworkReply> reply(http.openConnection(
                QUrl("http://fixture.invalid:47989"), "launch", {}, 120000, NvHTTP::NVLL_NONE));
            return QNetworkReply::NoError;
        } catch (const QtNetworkReplyException& error) {
            return error.error;
        }
    };

    // No DNS, socket, host or TLS operation is performed by this fake NAM.
    // A hanging request represents each asynchronous transport phase.
    for (const char* phase : { "DNS", "TCP", "TLS", "launch", "resume" }) {
        ScriptedNetwork network;
        NvHTTP http(&network);
        QDeadlineTimer budget(30, Qt::PreciseTimer);
        std::atomic<bool> cancelled(false);
        http.setRequestControl([&] { return static_cast<int>(budget.remainingTime()); },
                               &cancelled);
        // Guard only bounds a broken implementation's test runtime. It is well
        // beyond the actual budget and cannot make the deadline assertion pass.
        QTimer::singleShot(200, &network, [&] {
            for (auto* reply : network.findChildren<QNetworkReply*>())
                reply->abort();
        });
        QElapsedTimer clock;
        clock.start();
        const auto error = request(http);
        check(error == QNetworkReply::TimeoutError, "outage expiry must report timeout");
        check(clock.elapsed() < 150, phase);
    }

    {
        ScriptedNetwork network;
        NvHTTP http(&network);
        std::atomic<bool> cancelled(true);
        http.setRequestControl([] { return 60000; }, &cancelled);
        QTimer::singleShot(200, &network, [&] {
            for (auto* reply : network.findChildren<QNetworkReply*>())
                reply->abort();
        });
        check(request(http) == QNetworkReply::OperationCanceledError,
              "already cancelled attempt must report cancellation, not timeout");
        check(network.requests == 0, "cancelled attempt must not send a launch");
    }

    {
        ScriptedNetwork network;
        NvHTTP http(&network);
        std::atomic<bool> cancelled(false);
        http.setRequestControl([] { return 60000; }, &cancelled);
        QTimer::singleShot(30, &network, [&] { cancelled = true; });
        QTimer::singleShot(200, &network, [&] {
            for (auto* reply : network.findChildren<QNetworkReply*>())
                reply->abort();
        });
        QElapsedTimer clock;
        clock.start();
        check(request(http) == QNetworkReply::OperationCanceledError,
              "inflight cancellation must stay distinguishable from expiry");
        check(clock.elapsed() < 150, "cancel must abort pending transport promptly");
    }

    {
        ScriptedNetwork network;
        network.finishMs = 10;
        NvHTTP http(&network);
        std::atomic<bool> cancelled(false);
        http.setRequestControl(
            [&] {
                cancelled = true;
                return 60000;
            },
            &cancelled);
        check(request(http) == QNetworkReply::OperationCanceledError,
              "cancel arriving during preflight must win");
        check(network.requests == 0, "preflight cancel race must not send a launch");
    }

    {
        ScriptedNetwork network;
        network.finishMs = 10;
        NvHTTP http(&network);
        std::atomic<bool> cancelled(false);
        http.setRequestControl([] { return 0; }, &cancelled);
        check(request(http) == QNetworkReply::TimeoutError, "expired budget must fail preflight");
        check(network.requests == 0, "expired budget must not send another launch");
    }

    {
        ScriptedNetwork network;
        network.finishMs = 10;
        NvHTTP http(&network);
        check(request(http) == QNetworkReply::NoError, "uncontrolled legacy request must succeed");
        check(network.requests == 1, "legacy request must be sent exactly once");
    }

    if (failures)
        return 1;
    std::cout << "HTTP deadline and cancellation passed\n";
    return 0;
}
