#include "streaming/clipboardfilesreceiver.h"
#include "streaming/clipboardipc.h"
#include "streaming/clipboardsync.h"
#include "streaming/fileuploader.h"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPointer>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QtEndian>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

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
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

QByteArray kind5(const QByteArray& payload)
{
    QByteArray frame;
    frame.append(char(1));
    frame.append(char(ClipboardSync::KIND_FILE_LIST));
    const quint32 token = 0, size = qToLittleEndian(quint32(payload.size()));
    frame.append(reinterpret_cast<const char*>(&token), 4);
    frame.append(reinterpret_cast<const char*>(&size), 4);
    return frame + payload;
}

QString offerId(int n)
{
    return QString::number(n).rightJustified(64, QLatin1Char('c'));
}

QByteArray fileList(const QList<ClipboardFilesReceiver::Item>& items)
{
    QJsonArray array;
    qint64 total = 0;
    for (const auto& item : items) {
        array.append(QJsonObject{ { "id", item.id },
                                  { "name", item.name },
                                  { "rel", item.rel },
                                  { "size", static_cast<double>(item.size) } });
        total += item.size;
    }
    return QJsonDocument(QJsonObject{ { "intent", "clipboard" },
                                      { "count", static_cast<int>(items.size()) },
                                      { "total_size", static_cast<double>(total) },
                                      { "items", array } })
        .toJson(QJsonDocument::Compact);
}

// Serves offer downloads from an in-memory table (loopback; public test key).
class OfferHost : public QTcpServer
{
public:
    QSslCertificate certificate{ fixtureBytes("server.pem") };
    QSslKey key{ fixtureBytes("server-key.pem"), QSsl::Rsa };
    QHash<QString, QByteArray> files;
    QStringList requested;

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
            int end;
            while ((end = buffer->indexOf("\r\n\r\n")) >= 0) {
                const QByteArray line = buffer->left(buffer->indexOf("\r\n"));
                buffer->remove(0, end + 4);
                const QString id = QString::fromLatin1(line.split(' ').value(1)).section('/', -1);
                requested.append(id);
                const auto it = files.find(id);
                if (it == files.end()) {
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                } else {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                  "application/octet-stream\r\nContent-Length: " +
                                  QByteArray::number(it->size()) + "\r\n\r\n" + *it);
                }
                socket->flush();
            }
        });
        socket->startServerEncryption();
    }
};
} // namespace

bool runFileCopyTests(QTextStream& err)
{
    bool ok = true;
    auto* cb = QGuiApplication::clipboard();

#ifndef Q_OS_MACOS
    {
        QTemporaryDir dir;
        const QString a = dir.filePath("a.txt");
        QFile(a).open(QIODevice::WriteOnly);
        ClipboardSync sync;
        ClipboardSyncPolicy policy;
        policy.files = true;
        sync.setPolicy(policy);
        QList<QStringList> copied;
        QList<QByteArray> frames;
        QObject::connect(&sync, &ClipboardSync::localFilesCopied,
                         [&](QStringList p) { copied.append(p); });
        QObject::connect(&sync, &ClipboardSync::outboundFrame,
                         [&](QByteArray f) { frames.append(f); });
        sync.start();
        auto* mime = new QMimeData;
        mime->setUrls({ QUrl::fromLocalFile(a) });
        mime->setText(a);
        cb->setMimeData(mime);
        ok &= check(copied.size() == 1 && QFileInfo(copied[0].value(0)) == QFileInfo(a) &&
                        frames.isEmpty(),
                    "copied local files are reported, names never sent as text", err);

        sync.applyLocalFiles({ QUrl::fromLocalFile(a) }, sync.clipboardRevision());
        ok &= check(copied.size() == 1 && cb->mimeData()->hasUrls(),
                    "files we placed are not sent back", err);

        const quint64 stale = sync.clipboardRevision();
        cb->setText("newer");
        ok &=
            check(!sync.applyLocalFiles({ QUrl::fromLocalFile(a) }, stale) && cb->text() == "newer",
                  "a late download never replaces a newer clipboard", err);

        policy.files = false;
        sync.setPolicy(policy);
        auto* again = new QMimeData;
        again->setUrls({ QUrl::fromLocalFile(dir.filePath("b.txt")) });
        cb->setMimeData(again);
        ok &= check(copied.size() == 1, "files toggle off keeps file copies local", err);
    }
#endif

    {
        ClipboardSync sync;
        QList<QByteArray> lists;
        QObject::connect(&sync, &ClipboardSync::fileListReceived,
                         [&](QByteArray p) { lists.append(p); });
        sync.start();
        QMetaObject::invokeMethod(&sync, "onIncomingFrame", Qt::DirectConnection,
                                  Q_ARG(QByteArray, kind5("{\"intent\":\"clipboard\"}")));
        ok &= check(lists.isEmpty(), "host file lists ignored unless files are enabled", err);
        ClipboardSyncPolicy policy;
        policy.files = true;
        sync.setPolicy(policy);
        QMetaObject::invokeMethod(&sync, "onIncomingFrame", Qt::DirectConnection,
                                  Q_ARG(QByteArray, kind5("{\"intent\":\"clipboard\"}")));
        ok &= check(lists.size() == 1, "host file list forwarded when files are enabled", err);
    }

    {
        ClipboardIpc::HostConfig config;
        config.policy.files = true;
        ClipboardIpc::Message message;
        QString error;
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeConfigure(1, config), message, error) &&
                message.config.policy.files,
            "configure carries the files policy", err);
        const QByteArray legacy = R"({"type":"configure","sequence":2,"version":1,"host":{}})";
        ok &=
            check(ClipboardIpc::decodeLine(legacy, message, error) && !message.config.policy.files,
                  "file copy/paste stays off for older main processes", err);
    }

    {
        QList<ClipboardFilesReceiver::Item> items;
        QString error;
        ok &= check(ClipboardFilesReceiver::parseFileList(
                        fileList({ { offerId(1), "a.txt", "", 1 },
                                   { offerId(2), "g.sav", "Saves/slot", 2 } }),
                        items, error) &&
                        items.size() == 2 && items[1].rel == "Saves/slot",
                    "file list parses", err);
        ok &= check(!ClipboardFilesReceiver::parseFileList(fileList({ { "../x", "a", "", 1 } }),
                                                           items, error),
                    "bad offer id rejected", err);
        ok &= check(!ClipboardFilesReceiver::parseFileList(
                        fileList({ { offerId(1), "a", "../up", 1 } }), items, error),
                    "escaping folder rejected", err);
        ok &= check(!ClipboardFilesReceiver::parseFileList(
                        fileList({ { offerId(1), "a", "C:/x", 1 } }), items, error),
                    "absolute folder rejected", err);
        ok &= check(
            !ClipboardFilesReceiver::parseFileList(
                fileList({ { offerId(1), "a", "", ClipboardFilesReceiver::MAX_TOTAL_BYTES + 1 } }),
                items, error),
            "oversize list rejected", err);
        ok &= check(
            !ClipboardFilesReceiver::parseFileList(
                fileList({ { offerId(1), "Same.txt", "", 1 }, { offerId(2), "same.TXT", "", 1 } }),
                items, error),
            "colliding names rejected", err);
        ok &= check(ClipboardFilesReceiver::parseFileList(
                        fileList({ { offerId(1), "..\\evil.exe", "", 1 } }), items, error) &&
                        items[0].name == "evil.exe",
                    "names are sanitized", err);
    }

    OfferHost host;
    if (!check(QSslSocket::supportsSsl() && !host.certificate.isNull() &&
                   host.listen(QHostAddress::LocalHost),
               "offer host starts", err))
        return false;
    ClipboardSyncHostContext context;
    context.address = QStringLiteral("127.0.0.1");
    context.httpsPort = host.serverPort();
    context.serverCertificate = host.certificate;

    {
        QTemporaryDir root;
        host.files.insert(offerId(1), "hello");
        host.files.insert(offerId(2), "12");
        host.files.insert(offerId(3), "xyz");
        ClipboardFilesReceiver receiver(context);
        receiver.setRootDirectory(root.path());
        QList<QList<QUrl>> ready;
        QStringList failures;
        QObject::connect(&receiver, &ClipboardFilesReceiver::filesReady,
                         [&](QList<QUrl> u) { ready.append(u); });
        QObject::connect(&receiver, &ClipboardFilesReceiver::failed,
                         [&](QString e) { failures.append(e); });
        receiver.handleFileList(fileList({ { offerId(1), "a.txt", "", 5 },
                                           { offerId(2), "g.sav", "Saves/slot", 2 },
                                           { offerId(3), "h.sav", "Saves", 3 } }));
        ok &= check(waitUntil([&] { return !ready.isEmpty() || !failures.isEmpty(); }) &&
                        failures.isEmpty(),
                    "host files download", err);
        const QList<QUrl> urls = ready.value(0);
        ok &= check(urls.size() == 2 && urls[0].fileName() == "a.txt" &&
                        urls[1].fileName() == "Saves",
                    "clipboard gets the top-level entries", err);
        QFile nested(QDir(urls.value(1).toLocalFile()).filePath("slot/g.sav"));
        ok &= check(nested.open(QIODevice::ReadOnly) && nested.readAll() == "12",
                    "folders are recreated", err);
        nested.close();
#ifdef Q_OS_WIN
        QFile zone(urls.value(0).toLocalFile() + ":Zone.Identifier");
        ok &= check(zone.open(QIODevice::ReadOnly) && zone.readAll().contains("ZoneId=3"),
                    "host files are marked as downloaded", err);
#endif

        ready.clear();
        receiver.handleFileList(
            fileList({ { offerId(1), "a.txt", "", 5 }, { offerId(9), "gone.bin", "", 4 } }));
        ok &= check(waitUntil([&] { return !failures.isEmpty(); }) && ready.isEmpty(),
                    "a missing offer fails the whole paste", err);

        failures.clear();
        host.files.insert(offerId(4), "toolong");
        receiver.handleFileList(fileList({ { offerId(4), "short.bin", "", 3 } }));
        ok &= check(waitUntil([&] { return !failures.isEmpty(); }) && ready.isEmpty(),
                    "host sending more than announced fails", err);
    }

    {
        QTemporaryDir root;
        QDir(root.path()).mkpath("old-batch");
        QFile oldFile(root.filePath("old-batch/x.txt"));
        oldFile.open(QIODevice::WriteOnly);
        oldFile.close();
        QDir(root.path()).mkpath("recent-batch");
        const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
        std::filesystem::last_write_time(root.filePath("old-batch").toStdWString(), oldTime);
        host.files.insert(offerId(5), "k");
        ClipboardFilesReceiver receiver(context);
        receiver.setRootDirectory(root.path());
        int ready = 0;
        QObject::connect(&receiver, &ClipboardFilesReceiver::filesReady,
                         [&](QList<QUrl>) { ++ready; });
        receiver.handleFileList(fileList({ { offerId(5), "k.txt", "", 1 } }));
        ok &= check(waitUntil([&] { return ready == 1; }), "copy into persistent folder", err);
        ok &= check(!QDir(root.filePath("old-batch")).exists() &&
                        QDir(root.filePath("recent-batch")).exists(),
                    "copies older than a day are swept, recent ones kept", err);
    }
    ok &= check(QDir::cleanPath(ClipboardFilesReceiver::defaultRootDirectory()) !=
                    QDir::cleanPath(QDir::tempPath()),
                "copies live in a persistent app folder, not a temp folder deleted at exit", err);

    {
        QTemporaryDir dir;
        QFile big(dir.filePath("big.bin"));
        big.open(QIODevice::WriteOnly);
        big.resize(11);
        big.close();
        FileUploader uploader(context);
        QStringList failures;
        QObject::connect(&uploader, &FileUploader::failed,
                         [&](QString, QString e) { failures.append(e); });
        UploadRequest request;
        request.paths = { dir.filePath("big.bin") };
        request.intent = QStringLiteral("clipboard");
        request.maxTotalBytes = 10;
        uploader.start(request);
        ok &= check(failures.size() == 1 && !uploader.isBusy(),
                    "copy uploads respect their size cap", err);
    }

    return ok;
}
