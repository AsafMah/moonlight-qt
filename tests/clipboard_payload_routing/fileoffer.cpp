#include "streaming/clipboardipc.h"
#include "streaming/clipboardsync.h"
#include "streaming/fileofferreceiver.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QtEndian>

#include <functional>

namespace {
bool check(bool value, const char* name, QTextStream& err)
{
    if (!value) {
        err << "FAIL: " << name << '\n';
        err.flush();
    }
    return value;
}

QByteArray fixtureBytes(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool waitUntil(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

const QString kId =
    QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
const QString kId2 =
    QStringLiteral("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");

QByteArray offerJson(const QString& id, const QString& name, double size,
                     const QString& url = QString(), const QString& type = QStringLiteral("file"))
{
    QJsonObject obj{ { "id", id }, { "name", name }, { "size", size } };
    if (!type.isNull())
        obj.insert("type", type);
    obj.insert("download_url", url.isNull() ? QStringLiteral("/api/v1/file-transfer/") + id : url);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray wireFrame(int kind, const QByteArray& payload)
{
    QByteArray result;
    result.append(char(1));
    result.append(char(kind));
    const quint32 token = 0, size = qToLittleEndian(quint32(payload.size()));
    result.append(reinterpret_cast<const char*>(&token), 4);
    result.append(reinterpret_cast<const char*>(&size), 4);
    return result + payload;
}

// Loopback-only HTTPS server recording request lines. The key is a public test fixture.
class OfferServer : public QTcpServer
{
public:
    QSslCertificate certificate{ fixtureBytes("server.pem") };
    QSslKey key{ fixtureBytes("server-key.pem"), QSsl::Rsa };
    QList<QPointer<QSslSocket>> sockets;
    QList<QByteArray> requestLines;

    void respond(int index, const QByteArray& body, qint64 declaredLength = -1, int status = 200)
    {
        if (index >= sockets.size() || sockets[index].isNull())
            return;
        sockets[index]->write(
            "HTTP/1.1 " + QByteArray::number(status) +
            " OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
            QByteArray::number(declaredLength < 0 ? body.size() : declaredLength) + "\r\n\r\n" +
            body);
        sockets[index]->flush();
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            requestLines.append(buffer->left(buffer->indexOf("\r\n")));
            buffer->remove(0, end + 4);
            sockets.append(socket);
        });
        socket->startServerEncryption();
    }
};
} // namespace

bool runFileOfferTests(QTextStream& err)
{
    bool ok = true;

    {
        FileOffer offer;
        QString error;
        ok &= check(FileOfferReceiver::parseOffer(offerJson(kId, "save.zip", 5), offer, error) &&
                        offer.id == kId && offer.name == "save.zip" && offer.size == 5,
                    "valid offer parses", err);
        ok &= check(FileOfferReceiver::parseOffer(offerJson(kId, "a", 0, QString(), QString()),
                                                  offer, error),
                    "offer without type is a file", err);
        ok &= check(!FileOfferReceiver::parseOffer("{", offer, error), "bad JSON rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson(kId, "a", 1, QString(), "folder"),
                                                   offer, error),
                    "non-file offer rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson(kId.toUpper(), "a", 1), offer, error),
                    "uppercase id rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson("../../x", "a", 1), offer, error),
                    "path-like id rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson(kId, "a", -1), offer, error),
                    "negative size rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson(kId, "a", 1.5), offer, error),
                    "fractional size rejected", err);
        ok &= check(
            !FileOfferReceiver::parseOffer(
                offerJson(kId, "a", double(FileOfferReceiver::MAX_FILE_BYTES) + 1), offer, error),
            "oversize offer rejected", err);
        ok &= check(!FileOfferReceiver::parseOffer(offerJson(kId, "a", 1, "https://evil.example/x"),
                                                   offer, error),
                    "foreign download_url rejected", err);
        QJsonObject noSize{ { "id", kId }, { "name", "a" } };
        ok &= check(!FileOfferReceiver::parseOffer(QJsonDocument(noSize).toJson(), offer, error),
                    "missing size rejected", err);
    }

    {
        auto s = &FileOfferReceiver::sanitizeFileName;
        ok &= check(s("../../evil.exe") == "evil.exe", "parent traversal stripped", err);
        ok &= check(s("C:\\Windows\\system.dll") == "system.dll", "windows path stripped", err);
        ok &= check(s("a:b*c?.txt") == "a_b_c_.txt", "reserved characters replaced", err);
        ok &= check(s("notes.txt:hidden") == "notes.txt_hidden", "alternate data stream blocked",
                    err);
        ok &=
            check(s("CON") == "_CON" && s("con.txt") == "_con.txt" && s("LPT1.log") == "_LPT1.log",
                  "reserved device names prefixed", err);
        ok &= check(s(QString::fromUtf8("COM\xC2\xB9.bin")) ==
                            QString::fromUtf8("_COM\xC2\xB9.bin") &&
                        s(QString::fromUtf8("lpt\xC2\xB3")) == QString::fromUtf8("_lpt\xC2\xB3"),
                    "superscript device names prefixed", err);
        ok &= check(s("trail. . ") == "trail", "trailing dots and spaces removed", err);
        ok &= check(s("") == "download" && s("..") == "download" && s("/") == "download",
                    "empty names fall back", err);
        ok &= check(s(QString::fromUtf8("invoice\xE2\x80\xAEtxt.exe")) == "invoice_txt.exe",
                    "bidi override neutralised", err);
        const QString longName = s(QString(300, 'a') + ".txt");
        ok &= check(longName.size() <= FileOfferReceiver::MAX_FILE_NAME_CHARS &&
                        longName.endsWith(".txt"),
                    "long name truncated keeping extension", err);
        ok &= check(s(QString::fromUtf8("存档.sav")) == QString::fromUtf8("存档.sav"),
                    "unicode names preserved", err);
    }

    {
        QTemporaryDir dir;
        QFile(dir.filePath("a.txt")).open(QIODevice::WriteOnly);
        QFile(dir.filePath("a (1).txt.part")).open(QIODevice::WriteOnly);
        ok &=
            check(FileOfferReceiver::uniqueTargetPath(dir.path(), "b.txt") == dir.filePath("b.txt"),
                  "free name used as-is", err);
        ok &= check(FileOfferReceiver::uniqueTargetPath(dir.path(), "a.txt") ==
                        dir.filePath("a (2).txt"),
                    "existing file and pending part are not overwritten", err);
    }

    {
        ClipboardSync sync;
        QList<QByteArray> offers;
        QObject::connect(&sync, &ClipboardSync::fileOfferReceived,
                         [&](QByteArray payload) { offers.append(payload); });
        sync.start();
        const QByteArray payload = offerJson(kId, "x", 1);
        QMetaObject::invokeMethod(
            &sync, "onIncomingFrame", Qt::DirectConnection,
            Q_ARG(QByteArray, wireFrame(ClipboardSync::KIND_FILE_OFFER, payload)));
        ok &= check(offers.size() == 1 && offers[0] == payload, "kind 4 forwarded as file offer",
                    err);
        ClipboardSyncPolicy policy;
        policy.fileOffers = false;
        sync.setPolicy(policy);
        QMetaObject::invokeMethod(
            &sync, "onIncomingFrame", Qt::DirectConnection,
            Q_ARG(QByteArray, wireFrame(ClipboardSync::KIND_FILE_OFFER, payload)));
        ok &= check(offers.size() == 1, "file offers disabled by policy", err);
    }

    {
        ClipboardIpc::HostConfig config;
        config.policy.fileOffers = false;
        ClipboardIpc::Message message;
        QString error;
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeConfigure(1, config), message, error) &&
                !message.config.policy.fileOffers,
            "configure carries file offer policy", err);
        const QByteArray legacy = R"({"type":"configure","sequence":2,"version":1,"host":{}})";
        ok &= check(ClipboardIpc::decodeLine(legacy, message, error) &&
                        message.config.policy.fileOffers,
                    "legacy configure keeps file offers on", err);
        ClipboardIpc::TransferEvent event;
        event.state = ClipboardIpc::TransferState::Finished;
        event.id = kId;
        event.name = QString::fromUtf8("存档.sav");
        event.size = 5;
        event.path = QStringLiteral("C:/x/存档.sav");
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeTransferEvent(3, event), message, error) &&
                message.type == ClipboardIpc::MessageType::TransferEvent &&
                message.transfer.state == ClipboardIpc::TransferState::Finished &&
                message.transfer.id == kId && message.transfer.name == event.name &&
                message.transfer.size == 5 && message.transfer.path == event.path,
            "transfer event roundtrip", err);
        const QByteArray badState =
            R"({"type":"transferEvent","sequence":4,"version":1,"state":"exploded","id":"x"})";
        ok &= check(!ClipboardIpc::decodeLine(badState, message, error),
                    "unknown transfer state rejected", err);
    }

    OfferServer server;
    if (!check(QSslSocket::supportsSsl() && !server.certificate.isNull() && !server.key.isNull() &&
                   server.listen(QHostAddress::LocalHost),
               "file offer TLS fixture starts", err))
        return false;
    ClipboardSyncHostContext host;
    host.address = QStringLiteral("127.0.0.1");
    host.httpsPort = server.serverPort();
    host.serverCertificate = server.certificate;

    {
        QTemporaryDir dir;
        FileOfferReceiver receiver(host);
        receiver.setDownloadDirectory(dir.filePath("nested"));
        QStringList finished, failed;
        QObject::connect(&receiver, &FileOfferReceiver::transferFinished,
                         [&](QString, QString, QString path) { finished.append(path); });
        QObject::connect(&receiver, &FileOfferReceiver::transferFailed,
                         [&](QString, QString, QString error) { failed.append(error); });

        receiver.handleOffer(offerJson(kId, "../save.sav", 5));
        ok &= check(waitUntil([&] { return server.sockets.size() == 1; }) &&
                        server.requestLines.value(0) ==
                            "GET /api/v1/file-transfer/" + kId.toLatin1() + " HTTP/1.1",
                    "offer download requests the offer path over paired TLS", err);
        receiver.handleOffer(offerJson(kId, "../save.sav", 5));
        server.respond(0, "hello");
        ok &= check(waitUntil([&] { return !finished.isEmpty(); }) && failed.isEmpty(),
                    "offer download finishes", err);
        const QString saved = finished.value(0);
        QFile savedFile(saved);
        ok &= check(QFileInfo(saved).fileName() == "save.sav" &&
                        QFileInfo(saved).absolutePath() ==
                            QDir(dir.filePath("nested")).absolutePath() &&
                        savedFile.open(QIODevice::ReadOnly) && savedFile.readAll() == "hello" &&
                        !QFile::exists(saved + ".part"),
                    "downloaded file saved under sanitized name without leftovers", err);
        savedFile.close();
#ifdef Q_OS_WIN
        QFile zone(saved + ":Zone.Identifier");
        ok &= check(zone.open(QIODevice::ReadOnly) && zone.readAll().contains("ZoneId=3"),
                    "downloaded file carries Mark-of-the-Web", err);
#endif
        QThread::msleep(50);
        QCoreApplication::processEvents();
        ok &= check(server.sockets.size() == 1 && finished.size() == 1,
                    "duplicate offer id is not downloaded twice", err);

        receiver.handleOffer(offerJson(kId2, "short.bin", 5));
        ok &= check(waitUntil([&] { return server.sockets.size() == 2; }), "second offer requested",
                    err);
        server.respond(1, "abc");
        if (server.sockets.size() > 1 && !server.sockets[1].isNull())
            server.sockets[1]->disconnectFromHost();
        ok &= check(waitUntil([&] { return failed.size() == 1; }) && finished.size() == 1,
                    "short body fails", err);
        ok &= check(!QFile::exists(dir.filePath("nested/short.bin")) &&
                        !QFile::exists(dir.filePath("nested/short.bin.part")),
                    "failed download leaves no files", err);
    }

    {
        QTemporaryDir dir;
        FileOfferReceiver receiver(host);
        receiver.setDownloadDirectory(dir.path());
        QStringList failed;
        QObject::connect(&receiver, &FileOfferReceiver::transferFailed,
                         [&](QString, QString, QString error) { failed.append(error); });
        const int index = server.sockets.size();
        receiver.handleOffer(offerJson(kId, "over.bin", 3));
        ok &= check(waitUntil([&] { return server.sockets.size() > index; }), "oversend requested",
                    err);
        server.respond(index, "0123456789");
        ok &= check(waitUntil([&] { return failed.size() == 1; }) &&
                        !QFile::exists(dir.filePath("over.bin")) &&
                        !QFile::exists(dir.filePath("over.bin.part")),
                    "body larger than the offer is aborted and removed", err);
    }

    {
        QTemporaryDir dir;
        const int index = server.sockets.size();
        {
            FileOfferReceiver receiver(host);
            receiver.setDownloadDirectory(dir.path());
            receiver.handleOffer(offerJson(kId, "pending.bin", 5));
            ok &= check(waitUntil([&] { return server.sockets.size() > index; }) &&
                            QFile::exists(dir.filePath("pending.bin.part")),
                        "pending download writes a part file", err);
        }
        ok &= check(!QFile::exists(dir.filePath("pending.bin.part")),
                    "destroying the receiver removes partial downloads", err);
    }

    {
        QTemporaryDir dir;
        FileOfferReceiver receiver(host);
        receiver.setDownloadDirectory(dir.path());
        QStringList failed;
        QObject::connect(&receiver, &FileOfferReceiver::transferFailed,
                         [&](QString, QString, QString error) { failed.append(error); });
        const int index = server.sockets.size();
        receiver.handleOffer(offerJson(kId, "gone.bin", 5));
        ok &= check(waitUntil([&] { return server.sockets.size() > index; }), "404 requested", err);
        server.respond(index, "{\"error\":\"not_found\"}", -1, 404);
        ok &= check(waitUntil([&] { return failed.size() == 1; }) &&
                        !QFile::exists(dir.filePath("gone.bin")),
                    "HTTP error body is never saved", err);
    }

    {
        QTemporaryDir dir;
        FileOfferReceiver receiver(host);
        receiver.setDownloadDirectory(dir.path());
        receiver.setIdleTimeoutMs(300);
        QStringList failed;
        QObject::connect(&receiver, &FileOfferReceiver::transferFailed,
                         [&](QString, QString, QString error) { failed.append(error); });
        const int index = server.sockets.size();
        receiver.handleOffer(offerJson(kId, "stalled.bin", 5));
        ok &=
            check(waitUntil([&] { return server.sockets.size() > index; }), "stall requested", err);
        ok &= check(waitUntil([&] { return failed.size() == 1; }) &&
                        receiver.activeTransfers() == 0 &&
                        !QFile::exists(dir.filePath("stalled.bin.part")),
                    "stalled host times out and frees its slot", err);
    }

    {
        FileOfferReceiver receiver;
        QStringList failed;
        QObject::connect(&receiver, &FileOfferReceiver::transferFailed,
                         [&](QString, QString, QString error) { failed.append(error); });
        receiver.handleOffer(offerJson(kId, "a", 1));
        ok &= check(failed.size() == 1, "offer without host context fails visibly", err);
    }

    return ok;
}
