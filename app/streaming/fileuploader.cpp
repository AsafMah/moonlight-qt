#include "fileuploader.h"

#include "backend/pairedcertificate.h"
#include "clipboardlogging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QUuid>

#include <algorithm>

namespace {

const QString kApi = QStringLiteral("/api/v1/file-transfer");

bool isLink(const QFileInfo& info)
{
    return info.isSymLink() || info.isJunction();
}

// Appends every regular file below dir (sorted, links skipped) with its
// folder relative to the drop root.
bool collect(const QString& dir, const QString& rel, int depth, QList<UploadItem>& out,
             qint64& total, QString& error)
{
    if (depth > FileUploader::MAX_DEPTH) {
        error = QObject::tr("Folders are nested too deeply (limit %1 levels).")
                    .arg(FileUploader::MAX_DEPTH);
        return false;
    }
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                                      QDir::Hidden | QDir::System,
                                                  QDir::Name | QDir::DirsLast);
    for (const QFileInfo& entry : entries) {
        if (isLink(entry)) {
            continue;
        }
        if (entry.isDir()) {
            if (!collect(entry.absoluteFilePath(), rel + QLatin1Char('/') + entry.fileName(),
                         depth + 1, out, total, error)) {
                return false;
            }
            continue;
        }
        if (!entry.isFile()) {
            continue;
        }
        out.append({ entry.absoluteFilePath(), entry.fileName(), rel, entry.size() });
        total += entry.size();
        if (out.size() > FileUploader::MAX_FILES) {
            error = QObject::tr("Too many files (limit %1).").arg(FileUploader::MAX_FILES);
            return false;
        }
    }
    return true;
}

} // namespace

FileUploader::FileUploader(const ClipboardSyncHostContext& hostContext, QObject* parent)
    : QObject(parent), m_HostContext(hostContext)
{
}

FileUploader::~FileUploader()
{
    if (isBusy()) {
        if (m_Reply) {
            m_Reply->disconnect(this);
            m_Reply->abort();
            m_Reply->deleteLater();
        }
        delete m_File;
        m_File = nullptr;
    }
}

void FileUploader::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    const bool same = m_HostContext.address == hostContext.address &&
                      m_HostContext.httpsPort == hostContext.httpsPort &&
                      m_HostContext.serverCertificate == hostContext.serverCertificate &&
                      m_HostContext.clientCertificate == hostContext.clientCertificate &&
                      m_HostContext.clientPrivateKey == hostContext.clientPrivateKey;
    if (same) {
        return;
    }
    QNetworkReply* release = nullptr;
    if (isBusy()) {
        release = fail(tr("The host connection changed."));
    }
    if (m_Nam != nullptr) {
        // The batch DELETE for the old host travels on the old manager; keep it
        // alive until that request is done.
        QNetworkAccessManager* old = m_Nam;
        m_Nam = nullptr;
        if (release != nullptr) {
            connect(release, &QNetworkReply::finished, old, &QObject::deleteLater);
        } else {
            old->deleteLater();
        }
    }
    m_HostContext = hostContext;
}

void FileUploader::setIdleTimeoutMs(int timeoutMs)
{
    m_IdleTimeoutMs = timeoutMs;
}

void FileUploader::setChunkBytes(qint64 chunkBytes)
{
    m_ChunkBytes = std::max<qint64>(1, chunkBytes);
}

bool FileUploader::planUpload(const QStringList& paths, QList<UploadItem>& outItems,
                              QString& outError)
{
    outItems.clear();
    outError.clear();
    if (paths.isEmpty()) {
        outError = tr("Nothing to send.");
        return false;
    }

    qint64 total = 0;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            outError = tr("%1 doesn't exist.").arg(QDir::toNativeSeparators(path));
            return false;
        }
        if (isLink(info)) {
            continue;
        }
        if (info.isDir()) {
            if (!collect(info.absoluteFilePath(), info.fileName(), 1, outItems, total, outError)) {
                return false;
            }
        } else if (info.isFile()) {
            outItems.append({ info.absoluteFilePath(), info.fileName(), QString(), info.size() });
            total += info.size();
            if (outItems.size() > MAX_FILES) {
                outError = tr("Too many files (limit %1).").arg(MAX_FILES);
                return false;
            }
        }
    }

    if (outItems.isEmpty()) {
        outError = tr("There are no files to send.");
        return false;
    }
    for (const UploadItem& item : outItems) {
        if (item.size > MAX_FILE_BYTES) {
            outError = tr("%1 is larger than 4 GB.").arg(item.name);
            return false;
        }
        if (item.rel.toUtf8().size() > 240) {
            outError = tr("The folder path of %1 is too long.").arg(item.name);
            return false;
        }
    }
    if (total > MAX_TOTAL_BYTES) {
        outError = tr("The files add up to more than 16 GB.");
        return false;
    }
    return true;
}

bool FileUploader::isBusy() const
{
    return m_Stage != Stage::Idle;
}

void FileUploader::start(const UploadRequest& request)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (isBusy()) {
        emit failed(id, tr("Another file transfer is still running."));
        return;
    }

    QList<UploadItem> items;
    QString error;
    if (!planUpload(request.paths, items, error)) {
        emit failed(id, error);
        return;
    }
    QUrl probe;
    if (!buildUrl(kApi, QString(), probe)) {
        emit failed(id, tr("Not connected to a host."));
        return;
    }

    m_Id = id;
    m_Request = request;
    m_Items = items;
    m_TotalBytes = 0;
    for (const UploadItem& item : m_Items) {
        m_TotalBytes += item.size;
    }
    m_SentBytes = 0;
    m_Current = -1;
    m_BatchId.clear();
    m_UploadId.clear();
    m_Offset = 0;

    ClipboardLog::info("FileUploader: sending %d file(s), %lld bytes",
                       static_cast<int>(m_Items.size()), static_cast<long long>(m_TotalBytes));
    emit started(m_Id, m_Items.size(), m_TotalBytes);
    openBatch();
}

void FileUploader::cancel()
{
    if (isBusy()) {
        fail(tr("Cancelled."));
    }
}

QNetworkAccessManager* FileUploader::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        PairedCertificate::enforce(m_Nam, this, m_HostContext.serverCertificate);
        connect(
            m_Nam, &QNetworkAccessManager::sslErrors, this,
            [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                if (PairedCertificate::canIgnoreErrors(m_HostContext.serverCertificate, errors)) {
                    reply->ignoreSslErrors(errors);
                }
            });
    }
    return m_Nam;
}

bool FileUploader::buildUrl(const QString& path, const QString& query, QUrl& outUrl) const
{
    if (m_HostContext.address.isEmpty() || m_HostContext.httpsPort == 0 ||
        m_HostContext.serverCertificate.isNull()) {
        return false;
    }
    outUrl = QUrl();
    outUrl.setScheme(QStringLiteral("https"));
    outUrl.setHost(m_HostContext.address);
    outUrl.setPort(m_HostContext.httpsPort);
    outUrl.setPath(path);
    if (!query.isEmpty()) {
        outUrl.setQuery(query);
    }
    return outUrl.isValid();
}

QNetworkReply* FileUploader::send(const QByteArray& verb, const QString& path, const QString& query,
                                  const QByteArray& body, const QByteArray& contentType)
{
    QUrl url;
    buildUrl(path, query, url);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration ssl(QSslConfiguration::defaultConfiguration());
    ssl.setLocalCertificate(m_HostContext.clientCertificate);
    ssl.setPrivateKey(m_HostContext.clientPrivateKey);
    request.setSslConfiguration(ssl);
    request.setTransferTimeout(m_IdleTimeoutMs);
    if (!contentType.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    return nam()->sendCustomRequest(request, verb, body);
}

void FileUploader::openBatch()
{
    m_Stage = Stage::OpenBatch;
    const QJsonObject body{ { "count", static_cast<int>(m_Items.size()) },
                            { "total_size", static_cast<double>(m_TotalBytes) } };
    m_Reply = send("POST", kApi + QStringLiteral("/batches"), QString(),
                   QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
    connect(m_Reply, &QNetworkReply::finished, this,
            [this, reply = m_Reply.data()]() { onReply(reply); });
}

void FileUploader::createUpload()
{
    m_Stage = Stage::CreateUpload;
    const UploadItem& item = m_Items[m_Current];
    const QJsonObject body{ { "batch", m_BatchId },
                            { "name", item.name },
                            { "rel", item.rel },
                            { "size", static_cast<double>(item.size) } };
    m_Reply = send("POST", kApi + QStringLiteral("/uploads"), QString(),
                   QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
    connect(m_Reply, &QNetworkReply::finished, this,
            [this, reply = m_Reply.data()]() { onReply(reply); });
}

void FileUploader::sendChunk()
{
    m_Stage = Stage::SendChunk;
    const UploadItem& item = m_Items[m_Current];
    if (!m_File->seek(m_Offset)) {
        fail(tr("Couldn't read %1.").arg(item.name));
        return;
    }
    const QByteArray chunk = m_File->read(std::min(m_ChunkBytes, item.size - m_Offset));
    if (chunk.isEmpty() && item.size > m_Offset) {
        fail(tr("Couldn't read %1.").arg(item.name));
        return;
    }
    m_Reply = send("PUT", kApi + QStringLiteral("/uploads/") + m_UploadId,
                   QStringLiteral("offset=%1").arg(m_Offset), chunk, "application/octet-stream");
    connect(m_Reply, &QNetworkReply::finished, this,
            [this, reply = m_Reply.data()]() { onReply(reply); });
}

void FileUploader::commit()
{
    m_Stage = Stage::Commit;
    QJsonObject body{ { "intent", m_Request.intent } };
    if (m_Request.x) {
        body.insert(QStringLiteral("x"), *m_Request.x);
    }
    if (m_Request.y) {
        body.insert(QStringLiteral("y"), *m_Request.y);
    }
    m_Reply =
        send("POST", kApi + QStringLiteral("/batches/") + m_BatchId + QStringLiteral("/commit"),
             QString(), QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
    connect(m_Reply, &QNetworkReply::finished, this,
            [this, reply = m_Reply.data()]() { onReply(reply); });
}

void FileUploader::onReply(QNetworkReply* reply)
{
    reply->deleteLater();
    if (reply != m_Reply || !isBusy()) {
        return;
    }
    m_Reply = nullptr;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (status == 0) {
        fail(reply->error() == QNetworkReply::OperationCanceledError
                 ? tr("The host stopped responding.")
                 : tr("Couldn't reach the host: %1").arg(reply->errorString()));
        return;
    }
    const QJsonObject json = QJsonDocument::fromJson(body).object();

    switch (m_Stage) {
    case Stage::OpenBatch:
        if (status != 200 || json.value(QStringLiteral("batch")).toString().size() != 64) {
            fail(describeHttpError(status, body));
            return;
        }
        m_BatchId = json.value(QStringLiteral("batch")).toString();
        m_Current = 0;
        createUpload();
        return;

    case Stage::CreateUpload:
        if (status != 200 || json.value(QStringLiteral("id")).toString().size() != 64) {
            fail(describeHttpError(status, body));
            return;
        }
        m_UploadId = json.value(QStringLiteral("id")).toString();
        m_Offset = 0;
        delete m_File;
        m_File = new QFile(m_Items[m_Current].localPath);
        if (!m_File->open(QIODevice::ReadOnly)) {
            fail(tr("Couldn't open %1.").arg(m_Items[m_Current].name));
            return;
        }
        if (m_Items[m_Current].size == 0) {
            break;
        }
        sendChunk();
        return;

    case Stage::SendChunk: {
        const qint64 offset =
            static_cast<qint64>(json.value(QStringLiteral("offset")).toDouble(-1));
        const qint64 size = m_Items[m_Current].size;
        if ((status != 200 && status != 409) || offset < 0 || offset > size ||
            (status == 200 && offset <= m_Offset)) {
            fail(describeHttpError(status, body));
            return;
        }
        // 409: the host has a different offset (e.g. a lost response);
        // continue from what it actually stored.
        m_SentBytes += offset - m_Offset;
        m_Offset = offset;
        emit progress(m_Id, m_SentBytes, m_TotalBytes);
        if (m_Offset < size) {
            sendChunk();
            return;
        }
        break;
    }

    case Stage::Commit: {
        if (status != 200 || json.value(QStringLiteral("manifest")).toString().size() != 64) {
            fail(describeHttpError(status, body));
            return;
        }
        QJsonObject list{ { "manifest", json.value(QStringLiteral("manifest")) },
                          { "intent", m_Request.intent },
                          { "count", static_cast<int>(m_Items.size()) },
                          { "total_size", static_cast<double>(m_TotalBytes) } };
        if (m_Request.x) {
            list.insert(QStringLiteral("x"), *m_Request.x);
        }
        if (m_Request.y) {
            list.insert(QStringLiteral("y"), *m_Request.y);
        }
        const QString id = m_Id;
        const int count = m_Items.size();
        const qint64 total = m_TotalBytes;
        m_Stage = Stage::Idle;
        ClipboardLog::info("FileUploader: host accepted %d file(s)", count);
        emit fileListReady(QJsonDocument(list).toJson(QJsonDocument::Compact));
        emit finished(id, count, total);
        return;
    }

    case Stage::Idle:
        return;
    }

    // The current file is complete.
    delete m_File;
    m_File = nullptr;
    if (++m_Current < m_Items.size()) {
        createUpload();
    } else {
        commit();
    }
}

QNetworkReply* FileUploader::fail(const QString& error)
{
    const QString id = m_Id;
    if (m_Reply) {
        m_Reply->disconnect(this);
        m_Reply->abort();
        m_Reply->deleteLater();
        m_Reply = nullptr;
    }
    delete m_File;
    m_File = nullptr;
    QNetworkReply* release = abortRemote();
    m_Stage = Stage::Idle;
    ClipboardLog::warn("FileUploader: %s", error.toUtf8().constData());
    emit failed(id, error);
    return release;
}

QNetworkReply* FileUploader::abortRemote()
{
    if (m_BatchId.isEmpty()) {
        return nullptr;
    }
    QNetworkReply* reply = send("DELETE", kApi + QStringLiteral("/batches/") + m_BatchId, QString(),
                                QByteArray(), QByteArray());
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    m_BatchId.clear();
    return reply;
}

QString FileUploader::describeHttpError(int status, const QByteArray& body)
{
    const QString code =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
    if (code == QStringLiteral("file_upload_disabled")) {
        return tr(
            "The host doesn't accept files. Turn on \"Client-to-host file uploads\" in the host's "
            "Sunshine web settings (Input tab).");
    }
    if (status == 404 && code == QStringLiteral("not_found")) {
        return tr("This host doesn't support receiving files, or it forgot this transfer. Update "
                  "Foundation Sunshine on the host.");
    }
    if (code == QStringLiteral("no_session")) {
        return tr("The host doesn't see an active stream from this device.");
    }
    if (code == QStringLiteral("ambiguous_session")) {
        return tr("This device has more than one stream on the host.");
    }
    if (code == QStringLiteral("quota_exceeded") || code == QStringLiteral("disk_full")) {
        return tr("The host doesn't have room for these files.");
    }
    if (code == QStringLiteral("too_many")) {
        return tr("Too many file transfers are in progress on the host.");
    }
    if (code == QStringLiteral("too_large")) {
        return tr("The files are too large for the host.");
    }
    if (code == QStringLiteral("bad_name")) {
        return tr("The host rejected a file or folder name.");
    }
    return tr("The host answered with an error (HTTP %1%2).")
        .arg(status)
        .arg(code.isEmpty() ? QString() : QStringLiteral(", ") + code);
}
