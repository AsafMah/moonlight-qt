#include "streaming/clipboardipc.h"
#include "streaming/clipboardsync.h"
#include "streaming/fileuploader.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrlQuery>
#include <QtEndian>

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

QByteArray fixtureFile(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool waitFor(const std::function<bool()>& condition, int ms = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < ms) {
        // deleteLater() posted outside an event loop (calls made from this test's
        // top level) only runs on an explicit flush.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
}

QString hexId(int n)
{
    return QString::number(n).rightJustified(64, QLatin1Char('a'));
}

struct HttpRequest
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    QByteArray body;
};

// In-memory stand-in for Foundation Sunshine's upload API (loopback only; the
// key is a public test fixture).
class FakeUploadHost : public QTcpServer
{
public:
    QSslCertificate certificate{ fixtureFile("server.pem") };
    QSslKey key{ fixtureFile("server-key.pem"), QSsl::Rsa };

    QList<HttpRequest> requests;
    QJsonObject batchRequest;
    QList<QJsonObject> uploadRequests;
    QHash<QString, QByteArray> received; // upload id -> bytes
    QJsonObject commitRequest;
    int batchStatus = 200;
    QByteArray batchError = "file_upload_disabled";
    int mismatchOncePuts = -1; // index of PUT answered with 409 + current offset
    bool stallPuts = false;
    int nextId = 1;

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
            while (true) {
                const int end = buffer->indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                const QList<QByteArray> lines = buffer->left(end).split('\n');
                int length = 0;
                for (const QByteArray& line : lines) {
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                }
                if (buffer->size() < end + 4 + length)
                    return;
                const QList<QByteArray> start = lines[0].trimmed().split(' ');
                HttpRequest r;
                r.method = start.value(0);
                const QUrl url(QString::fromLatin1(start.value(1)));
                r.path = url.path();
                r.query = QUrlQuery(url.query());
                r.body = buffer->mid(end + 4, length);
                buffer->remove(0, end + 4 + length);
                requests.append(r);
                answer(socket, r);
            }
        });
        socket->startServerEncryption();
    }

private:
    void reply(QSslSocket* socket, int status, const QJsonObject& body)
    {
        const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) +
                      " X\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(json.size()) + "\r\n\r\n" + json);
        socket->flush();
    }

    void answer(QSslSocket* socket, const HttpRequest& r)
    {
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();
        if (r.method == "POST" && r.path == "/api/v1/file-transfer/batches") {
            batchRequest = body;
            if (batchStatus != 200) {
                reply(socket, batchStatus, { { "error", QString::fromLatin1(batchError) } });
                return;
            }
            reply(socket, 200, { { "batch", hexId(nextId++) } });
            return;
        }
        if (r.method == "POST" && r.path == "/api/v1/file-transfer/uploads") {
            uploadRequests.append(body);
            const QString id = hexId(nextId++);
            received.insert(id, QByteArray());
            reply(socket, 200,
                  { { "id", id }, { "chunk_size", 4 * 1024 * 1024 }, { "offset", 0 } });
            return;
        }
        if (r.method == "PUT" && r.path.startsWith("/api/v1/file-transfer/uploads/")) {
            const QString id = r.path.section('/', -1);
            int putIndex = 0;
            for (const auto& prior : requests)
                putIndex += prior.method == "PUT";
            if (stallPuts)
                return;
            QByteArray& bytes = received[id];
            if (putIndex - 1 == mismatchOncePuts ||
                r.query.queryItemValue("offset").toLongLong() != bytes.size()) {
                reply(socket, 409, { { "error", "offset_mismatch" }, { "offset", bytes.size() } });
                return;
            }
            bytes += r.body;
            reply(socket, 200, { { "offset", bytes.size() } });
            return;
        }
        if (r.method == "POST" && r.path.endsWith("/commit")) {
            commitRequest = body;
            reply(socket, 200,
                  { { "manifest", hexId(999) },
                    { "count", uploadRequests.size() },
                    { "total_size", 10 } });
            return;
        }
        if (r.method == "DELETE") {
            socket->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
            socket->flush();
            return;
        }
        reply(socket, 404, { { "error", "not_found" } });
    }
};

struct Recorder
{
    QStringList failures;
    QList<QByteArray> fileLists;
    int finished = 0;
    qint64 lastProgress = -1;
    bool progressMonotonic = true;

    void attach(FileUploader& uploader)
    {
        QObject::connect(&uploader, &FileUploader::failed,
                         [this](QString, QString error) { failures.append(error); });
        QObject::connect(&uploader, &FileUploader::fileListReady,
                         [this](QByteArray payload) { fileLists.append(payload); });
        QObject::connect(&uploader, &FileUploader::finished,
                         [this](QString, int, qint64) { ++finished; });
        QObject::connect(&uploader, &FileUploader::progress, [this](QString, qint64 sent, qint64) {
            progressMonotonic &= sent >= lastProgress;
            lastProgress = sent;
        });
    }
};
} // namespace

bool runFileUploadTests(QTextStream& err)
{
    bool ok = true;

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.txt"), "hello");
        writeFile(dir.filePath("Saves/slot1/game.sav"), "12345");
        writeFile(dir.filePath("Saves/empty.dat"), "");
        QList<UploadItem> items;
        QString error;
        ok &= check(FileUploader::planUpload({ dir.filePath("a.txt"), dir.filePath("Saves") },
                                             items, error) &&
                        items.size() == 3,
                    "plan expands dropped folder", err);
        bool sawNested = false;
        for (const auto& item : items) {
            sawNested |= item.name == "game.sav" && item.rel == "Saves/slot1" && item.size == 5;
        }
        ok &= check(sawNested, "plan keeps folder structure", err);
        ok &= check(items.value(0).name == "a.txt" && items.value(0).rel.isEmpty(),
                    "plan keeps loose files at the root", err);

#ifdef Q_OS_WIN
        QProcess mk;
        mk.start("cmd",
                 { "/c", "mklink", "/J", QDir::toNativeSeparators(dir.filePath("Saves/link")),
                   QDir::toNativeSeparators(dir.path()) });
        mk.waitForFinished();
        ok &= check(FileUploader::planUpload({ dir.filePath("Saves") }, items, error) &&
                        items.size() == 2,
                    "plan never follows junctions", err);
#endif
        ok &= check(!FileUploader::planUpload({}, items, error), "empty drop rejected", err);
        ok &= check(!FileUploader::planUpload({ dir.filePath("missing") }, items, error) &&
                        !error.isEmpty(),
                    "missing path rejected", err);
        QString deep = dir.filePath("d");
        for (int i = 0; i < FileUploader::MAX_DEPTH + 1; ++i)
            deep += "/d";
        writeFile(deep + "/f", "x");
        ok &= check(!FileUploader::planUpload({ dir.filePath("d") }, items, error),
                    "too deep rejected", err);
    }

    {
        QTemporaryDir dir;
        for (int i = 0; i <= FileUploader::MAX_FILES; ++i)
            writeFile(dir.filePath(QStringLiteral("many/%1").arg(i)), "");
        QList<UploadItem> items;
        QString error;
        ok &= check(!FileUploader::planUpload({ dir.filePath("many") }, items, error) &&
                        error.contains("1000"),
                    "too many files rejected", err);
    }

    {
        ClipboardSync sync;
        QList<QByteArray> sent;
        QObject::connect(&sync, &ClipboardSync::outboundFrame,
                         [&](QByteArray b) { sent.append(b); });
        sync.sendFileList("{\"manifest\":\"x\"}");
        ok &= check(sent.size() == 1 && sent[0][1] == ClipboardSync::KIND_FILE_LIST &&
                        sent[0].mid(10) == "{\"manifest\":\"x\"}",
                    "file list sent as kind 5", err);
    }

    {
        ClipboardIpc::UploadFiles upload;
        upload.paths = { QStringLiteral("C:/a b/存档.sav"), QStringLiteral("D:/x") };
        upload.intent = QStringLiteral("drop");
        upload.x = 0.25;
        upload.y = 0.75;
        ClipboardIpc::Message message;
        QString error;
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeUploadFiles(1, upload), message, error) &&
                message.type == ClipboardIpc::MessageType::UploadFiles &&
                message.upload.paths == upload.paths && message.upload.x == 0.25 &&
                message.upload.y == 0.75 && message.upload.intent == "drop",
            "uploadFiles IPC roundtrip", err);
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeCancelTransfers(2), message, error) &&
                message.type == ClipboardIpc::MessageType::CancelTransfers,
            "cancelTransfers IPC roundtrip", err);
        ClipboardIpc::TransferEvent event;
        event.state = ClipboardIpc::TransferState::Progress;
        event.direction = ClipboardIpc::TransferDirection::Upload;
        event.count = 3;
        event.bytes = 40;
        event.size = 100;
        ok &= check(
            ClipboardIpc::decodeLine(ClipboardIpc::encodeTransferEvent(3, event), message, error) &&
                message.transfer.state == ClipboardIpc::TransferState::Progress &&
                message.transfer.direction == ClipboardIpc::TransferDirection::Upload &&
                message.transfer.count == 3 && message.transfer.bytes == 40 &&
                message.transfer.size == 100,
            "upload progress event roundtrip", err);
        const QByteArray badPaths =
            R"({"type":"uploadFiles","sequence":4,"version":1,"intent":"drop","paths":"C:/x"})";
        ok &= check(!ClipboardIpc::decodeLine(badPaths, message, error),
                    "uploadFiles needs a path list", err);
    }

    FakeUploadHost host;
    if (!check(QSslSocket::supportsSsl() && !host.certificate.isNull() &&
                   host.listen(QHostAddress::LocalHost),
               "upload TLS fixture starts", err))
        return false;
    ClipboardSyncHostContext context;
    context.address = QStringLiteral("127.0.0.1");
    context.httpsPort = host.serverPort();
    context.serverCertificate = host.certificate;

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("Saves/game.sav"), "0123456789");
        writeFile(dir.filePath("Saves/empty.dat"), "");
        FileUploader uploader(context);
        uploader.setChunkBytes(4);
        Recorder rec;
        rec.attach(uploader);
        UploadRequest request;
        request.paths = { dir.filePath("Saves") };
        request.x = 0.5;
        request.y = 0.25;
        uploader.start(request);
        ok &= check(uploader.isBusy(), "uploader busy after start", err);
        uploader.start(request);
        ok &= check(rec.failures.size() == 1, "second drop while busy is refused", err);
        ok &= check(waitFor([&] { return rec.finished == 1 || rec.failures.size() > 1; }) &&
                        rec.failures.size() == 1,
                    "upload finishes", err);
        ok &= check(host.batchRequest.value("count").toInt() == 2 &&
                        host.batchRequest.value("total_size").toInt() == 10,
                    "batch declares count and total", err);
        bool relOk = host.uploadRequests.size() == 2;
        for (const auto& u : host.uploadRequests)
            relOk &= u.value("rel").toString() == "Saves";
        ok &= check(relOk, "uploads carry the folder", err);
        QByteArray all;
        for (const auto& bytes : host.received)
            all += bytes;
        ok &= check(all == "0123456789", "host received the exact bytes", err);
        int puts = 0;
        for (const auto& r : host.requests)
            puts += r.method == "PUT";
        ok &= check(puts == 3, "10 bytes sent as three 4-byte-max chunks", err);
        ok &= check(host.commitRequest.value("intent").toString() == "drop" &&
                        host.commitRequest.value("x").toDouble() == 0.5,
                    "commit carries intent and drop position", err);
        const QJsonObject list = QJsonDocument::fromJson(rec.fileLists.value(0)).object();
        ok &= check(rec.fileLists.size() == 1 && list.value("manifest").toString() == hexId(999) &&
                        list.value("intent").toString() == "drop" &&
                        list.value("count").toInt() == 2 && list.value("y").toDouble() == 0.25,
                    "file list payload names the manifest", err);
        ok &= check(rec.progressMonotonic && rec.lastProgress == 10, "progress reaches total", err);
        ok &= check(!uploader.isBusy(), "uploader idle after finishing", err);
    }

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("f.bin"), "0123456789");
        host.requests.clear();
        host.received.clear();
        host.uploadRequests.clear();
        host.mismatchOncePuts = 1;
        FileUploader uploader(context);
        uploader.setChunkBytes(4);
        Recorder rec;
        rec.attach(uploader);
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] { return rec.finished == 1 || !rec.failures.isEmpty(); }) &&
                        rec.failures.isEmpty() && host.received.values().value(0) == "0123456789",
                    "offset mismatch resyncs from the host offset", err);
        host.mismatchOncePuts = -1;
    }

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("f.bin"), "x");
        host.batchStatus = 403;
        FileUploader uploader(context);
        Recorder rec;
        rec.attach(uploader);
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] { return !rec.failures.isEmpty(); }) &&
                        rec.failures[0].contains("host", Qt::CaseInsensitive),
                    "disabled host explains the host setting", err);
        host.batchStatus = 404;
        host.batchError = "not_found";
        rec.failures.clear();
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] { return !rec.failures.isEmpty(); }) &&
                        rec.failures[0].contains("support", Qt::CaseInsensitive),
                    "old host reports missing support", err);
        host.batchStatus = 200;
    }

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("f.bin"), "0123456789");
        host.requests.clear();
        host.stallPuts = true;
        FileUploader uploader(context);
        uploader.setChunkBytes(4);
        Recorder rec;
        rec.attach(uploader);
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] {
                        for (const auto& r : host.requests)
                            if (r.method == "PUT")
                                return true;
                        return false;
                    }),
                    "stalled upload reached the first chunk", err);
        uploader.cancel();
        ok &= check(rec.failures.size() == 1 && !uploader.isBusy(), "cancel stops the upload", err);
        ok &= check(waitFor([&] {
                        for (const auto& r : host.requests)
                            if (r.method == "DELETE" && r.path.contains("/batches/"))
                                return true;
                        return false;
                    }),
                    "cancel deletes the batch on the host", err);

        rec.failures.clear();
        uploader.setIdleTimeoutMs(300);
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] { return !rec.failures.isEmpty(); }) && !uploader.isBusy(),
                    "stalled host times out", err);
        ok &= check(waitFor([&] { return uploader.findChildren<QNetworkReply*>().isEmpty(); }),
                    "cancelled and timed-out requests are freed", err);

        host.requests.clear();
        rec.failures.clear();
        uploader.setIdleTimeoutMs(30000);
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(waitFor([&] {
                        for (const auto& r : host.requests)
                            if (r.method == "PUT")
                                return true;
                        return false;
                    }),
                    "upload in flight before host change", err);
        ClipboardSyncHostContext other = context;
        other.httpsPort = 1;
        uploader.setHostContext(other);
        ok &= check(rec.failures.size() == 1, "host change fails the upload", err);
        ok &= check(waitFor([&] {
                        for (const auto& r : host.requests)
                            if (r.method == "DELETE" && r.path.contains("/batches/"))
                                return true;
                        return false;
                    }),
                    "host change still releases the batch on the old host", err);
        host.stallPuts = false;
    }

    {
        FileUploader uploader;
        Recorder rec;
        rec.attach(uploader);
        QTemporaryDir dir;
        writeFile(dir.filePath("f.bin"), "x");
        uploader.start({ { dir.filePath("f.bin") } });
        ok &= check(rec.failures.size() == 1, "upload without host connection fails visibly", err);
    }

    {
        QTemporaryDir dir;
        writeFile(dir.filePath("a.bin"), "0123456789");
        writeFile(dir.filePath("b.bin"), "0123456789");
        host.requests.clear();
        host.stallPuts = true;
        FileUploader uploader(context);
        uploader.setChunkBytes(4);
        Recorder rec;
        QStringList superseded;
        rec.attach(uploader);
        QObject::connect(&uploader, &FileUploader::superseded,
                         [&](QString id) { superseded.append(id); });
        auto batches = [&] {
            int n = 0;
            for (const auto& r : host.requests)
                n += r.method == "POST" && r.path.endsWith("/batches");
            return n;
        };
        auto puts = [&] {
            int n = 0;
            for (const auto& r : host.requests)
                n += r.method == "PUT";
            return n;
        };
        UploadRequest copyA{ { dir.filePath("a.bin") }, QStringLiteral("clipboard") };
        UploadRequest copyB{ { dir.filePath("b.bin") }, QStringLiteral("clipboard") };
        uploader.start(copyA);
        ok &= check(waitFor([&] { return puts() == 1; }), "first copy upload running", err);
        uploader.start(copyB);
        ok &= check(superseded.size() == 1 && rec.failures.isEmpty() && uploader.isBusy(),
                    "a newer copy silently replaces a running copy upload", err);
        ok &= check(waitFor([&] { return batches() == 2 && puts() == 2; }),
                    "the newer copy uploads", err);

        uploader.cancel();
        rec.failures.clear();
        host.requests.clear();
        uploader.start({ { dir.filePath("a.bin") } });
        ok &= check(waitFor([&] { return puts() == 1; }), "drop upload running", err);
        uploader.start(copyB);
        ok &= check(rec.failures.isEmpty() && batches() == 1, "a copy during a drop waits", err);
        uploader.cancel();
        ok &= check(waitFor([&] { return batches() == 2; }),
                    "the waiting copy starts after the drop ends", err);
        uploader.cancel();
        host.stallPuts = false;
    }

    return ok;
}
