#include "hostcommandrunner.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"

#include <QReadLocker>
#include <QThread>

#include <memory>

HostCommandRunner::HostCommandRunner(QObject* parent) : QObject(parent) {}

bool HostCommandRunner::run(NvComputer* computer, const QString& commandId, Callback callback)
{
    if (m_Busy || computer == nullptr) {
        return false;
    }

    NvAddress address;
    uint16_t httpsPort;
    QString uuid;
    QSslCertificate certificate;
    {
        QReadLocker locker(&computer->lock);
        address = computer->activeAddress;
        httpsPort = computer->activeHttpsPort;
        uuid = computer->uuid;
        certificate = computer->serverCert;
    }

    struct Result
    {
        bool accepted = false;
        QString error;
    };
    auto result = std::make_shared<Result>();

    QThread* worker = QThread::create([result, address, httpsPort, uuid, certificate, commandId] {
        try {
            NvHTTP http(address, httpsPort, certificate, true, nullptr, uuid);
            result->accepted = http.sendSuperCmd(commandId);
        } catch (const GfeHttpResponseException& e) {
            result->error = e.toQString();
        } catch (const QtNetworkReplyException& e) {
            result->error = e.toQString();
        } catch (const std::exception& e) {
            result->error = QString::fromUtf8(e.what());
        }
    });

    m_Busy = true;
    connect(worker, &QThread::finished, this, [this, result, callback = std::move(callback)] {
        m_Busy = false;
        if (callback) {
            callback(result->accepted, result->error);
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return true;
}
