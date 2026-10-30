#include "clipboardfilesreceiver.h"

#include "backend/pairedcertificate.h"
#include "clipboardlogging.h"
#include "fileofferreceiver.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSslConfiguration>
#include <QStandardPaths>

#include <cmath>

namespace {

constexpr int kMaxRelDepth = 8;
constexpr int kMaxRelBytes = 240;

bool isOfferId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[a-f0-9]{64}$"));
    return re.match(id).hasMatch();
}

// '/'-joined safe relative folder, or a null string when unsafe.
QString validateRel(const QString& rel)
{
    if (rel.isEmpty()) {
        return QStringLiteral("");
    }
    if (rel.toUtf8().size() > kMaxRelBytes) {
        return QString();
    }
    const QStringList segments = rel.split(QRegularExpression(QStringLiteral("[/\\\\]")));
    if (segments.size() > kMaxRelDepth) {
        return QString();
    }
    for (const QString& segment : segments) {
        // A segment must survive name sanitizing unchanged: that rejects
        // empty parts ("/abs", "a//b"), "..", drive letters, ADS colons,
        // reserved device names and trailing dots or spaces.
        if (segment.isEmpty() || segment.startsWith(QLatin1Char(' ')) ||
            FileOfferReceiver::sanitizeFileName(segment) != segment) {
            return QString();
        }
    }
    return segments.join(QLatin1Char('/'));
}

} // namespace

ClipboardFilesReceiver::ClipboardFilesReceiver(const ClipboardSyncHostContext& hostContext,
                                               QObject* parent)
    : QObject(parent), m_HostContext(hostContext)
{
}

ClipboardFilesReceiver::~ClipboardFilesReceiver()
{
    abortCurrent();
}

void ClipboardFilesReceiver::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    if (m_HostContext.address == hostContext.address &&
        m_HostContext.httpsPort == hostContext.httpsPort &&
        m_HostContext.serverCertificate == hostContext.serverCertificate &&
        m_HostContext.clientCertificate == hostContext.clientCertificate &&
        m_HostContext.clientPrivateKey == hostContext.clientPrivateKey) {
        return;
    }
    if (isBusy()) {
        fail(tr("The host connection changed."));
    }
    if (m_Nam != nullptr) {
        m_Nam->deleteLater();
        m_Nam = nullptr;
    }
    m_HostContext = hostContext;
}

void ClipboardFilesReceiver::setIdleTimeoutMs(int timeoutMs)
{
    m_IdleTimeoutMs = timeoutMs;
}

void ClipboardFilesReceiver::setRootDirectory(const QString& directory)
{
    m_RootOverride = directory;
}

bool ClipboardFilesReceiver::isBusy() const
{
    return m_Current >= 0;
}

bool ClipboardFilesReceiver::parseFileList(const QByteArray& payload, QList<Item>& outItems,
                                           QString& outError)
{
    outItems.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
    if (!doc.isObject() || doc.object().value(QStringLiteral("intent")).toString() != "clipboard" ||
        items.isEmpty() || items.size() > MAX_ITEMS) {
        outError = tr("The host sent an invalid file list.");
        return false;
    }

    qint64 total = 0;
    QSet<QString> seen;
    for (const QJsonValue& value : items) {
        const QJsonObject obj = value.toObject();
        Item item;
        item.id = obj.value(QStringLiteral("id")).toString();
        item.name =
            FileOfferReceiver::sanitizeFileName(obj.value(QStringLiteral("name")).toString());
        item.rel = validateRel(obj.value(QStringLiteral("rel")).toString());
        const double size = obj.value(QStringLiteral("size")).toDouble(-1);
        if (!isOfferId(item.id) || item.rel.isNull() || !std::isfinite(size) || size < 0 ||
            std::floor(size) != size || size > MAX_TOTAL_BYTES) {
            outError = tr("The host sent an invalid file list.");
            return false;
        }
        item.size = static_cast<qint64>(size);
        total += item.size;
        if (total > MAX_TOTAL_BYTES) {
            outError =
                tr("The copied files are larger than %1 MB.").arg(MAX_TOTAL_BYTES / (1024 * 1024));
            return false;
        }
        const QString key =
            (item.rel.isEmpty() ? item.name : item.rel + QLatin1Char('/') + item.name).toLower();
        if (seen.contains(key)) {
            outError = tr("The host sent two files with the same name.");
            return false;
        }
        seen.insert(key);
        outItems.append(item);
    }
    return true;
}

QString ClipboardFilesReceiver::defaultRootDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.moonlight");
    }
    return QDir(base).filePath(QStringLiteral("Clipboard Files"));
}

QString ClipboardFilesReceiver::rootDirectory()
{
    const QString root = m_RootOverride.isEmpty() ? defaultRootDirectory() : m_RootOverride;
    if (!QDir().mkpath(root)) {
        return QString();
    }
    if (!m_Swept) {
        m_Swept = true;
        const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-KEEP_HOURS * 3600);
        const QFileInfoList batches =
            QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& batch : batches) {
            if (batch.lastModified() < cutoff) {
                QDir(batch.absoluteFilePath()).removeRecursively();
            }
        }
    }
    return root;
}

void ClipboardFilesReceiver::handleFileList(const QByteArray& payload)
{
    if (isBusy()) {
        // The user copied something newer on the host.
        abortCurrent();
        QDir(m_BatchDir).removeRecursively();
        m_Current = -1;
    }

    QList<Item> items;
    QString error;
    if (!parseFileList(payload, items, error)) {
        emit failed(error);
        return;
    }
    const QString root = rootDirectory();
    if (root.isEmpty()) {
        emit failed(tr("Couldn't create a temporary folder."));
        return;
    }

    // Each paste gets its own folder so earlier copies stay valid.
    QDir rootDir(root);
    const QString base = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString batch = base;
    for (int n = 1; rootDir.exists(batch); ++n) {
        batch = QStringLiteral("%1-%2").arg(base).arg(n);
    }
    if (!rootDir.mkpath(batch)) {
        emit failed(tr("Couldn't create a temporary folder."));
        return;
    }
    m_BatchDir = rootDir.filePath(batch);
    m_Items = items;
    m_TopLevel.clear();
    m_Current = 0;
    ClipboardLog::info("ClipboardFilesReceiver: receiving %d copied file(s)",
                       static_cast<int>(items.size()));
    next();
}

QNetworkAccessManager* ClipboardFilesReceiver::nam()
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

void ClipboardFilesReceiver::next()
{
    if (m_Current >= m_Items.size()) {
        const QList<QUrl> urls = m_TopLevel;
        m_Current = -1;
        emit filesReady(urls);
        return;
    }

    const Item& item = m_Items[m_Current];
    if (m_HostContext.address.isEmpty() || m_HostContext.httpsPort == 0 ||
        m_HostContext.serverCertificate.isNull()) {
        fail(tr("Not connected to a host."));
        return;
    }

    QDir batch(m_BatchDir);
    if (!item.rel.isEmpty() && !batch.mkpath(item.rel)) {
        fail(tr("Couldn't create %1.").arg(item.rel));
        return;
    }
    const QString target =
        batch.filePath(item.rel.isEmpty() ? item.name : item.rel + QLatin1Char('/') + item.name);
    const QString top = item.rel.isEmpty() ? item.name : item.rel.section(QLatin1Char('/'), 0, 0);
    const QUrl topUrl = QUrl::fromLocalFile(batch.filePath(top));
    if (!m_TopLevel.contains(topUrl)) {
        m_TopLevel.append(topUrl);
    }

    m_File = new QFile(target);
    if (!m_File->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(tr("Couldn't write %1.").arg(item.name));
        return;
    }
    m_Written = 0;
    m_Failure.clear();

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(m_HostContext.address);
    url.setPort(m_HostContext.httpsPort);
    url.setPath(QStringLiteral("/api/v1/file-transfer/") + item.id);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration ssl(QSslConfiguration::defaultConfiguration());
    ssl.setLocalCertificate(m_HostContext.clientCertificate);
    ssl.setPrivateKey(m_HostContext.clientPrivateKey);
    request.setSslConfiguration(ssl);
    request.setTransferTimeout(m_IdleTimeoutMs);
    m_Reply = nam()->get(request);
    connect(m_Reply, &QNetworkReply::readyRead, this, &ClipboardFilesReceiver::onReadyRead);
    connect(m_Reply, &QNetworkReply::finished, this, &ClipboardFilesReceiver::onFinished);
}

void ClipboardFilesReceiver::onReadyRead()
{
    if (!m_Reply || !m_Failure.isEmpty()) {
        return;
    }
    if (m_Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        m_Failure = tr("The host no longer offers %1.").arg(m_Items[m_Current].name);
        return;
    }
    const QByteArray chunk = m_Reply->readAll();
    if (m_Written + chunk.size() > m_Items[m_Current].size) {
        m_Failure = tr("The host sent more data than announced.");
        m_Reply->abort();
        return;
    }
    if (m_File->write(chunk) != chunk.size()) {
        m_Failure = m_File->errorString();
        m_Reply->abort();
        return;
    }
    m_Written += chunk.size();
}

void ClipboardFilesReceiver::onFinished()
{
    QNetworkReply* reply = m_Reply;
    if (reply == nullptr) {
        return;
    }
    if (m_Failure.isEmpty() && reply->bytesAvailable() > 0) {
        onReadyRead();
    }
    reply->deleteLater();
    m_Reply = nullptr;

    QString failure = m_Failure;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (failure.isEmpty() && status != 200) {
        failure = status == 0 ? tr("Couldn't reach the host: %1").arg(reply->errorString())
                              : tr("The host no longer offers %1.").arg(m_Items[m_Current].name);
    }
    if (failure.isEmpty() && m_Written != m_Items[m_Current].size) {
        failure = tr("%1 arrived incomplete.").arg(m_Items[m_Current].name);
    }
    const QString path = m_File->fileName();
    m_File->close();
    delete m_File;
    m_File = nullptr;
    if (!failure.isEmpty()) {
        fail(failure);
        return;
    }
    FileOfferReceiver::markFromInternet(path);
    ++m_Current;
    next();
}

void ClipboardFilesReceiver::abortCurrent()
{
    if (m_Reply) {
        m_Reply->disconnect(this);
        m_Reply->abort();
        m_Reply->deleteLater();
        m_Reply = nullptr;
    }
    if (m_File != nullptr) {
        m_File->close();
        delete m_File;
        m_File = nullptr;
    }
}

void ClipboardFilesReceiver::fail(const QString& error)
{
    abortCurrent();
    if (!m_BatchDir.isEmpty()) {
        QDir(m_BatchDir).removeRecursively();
    }
    m_Current = -1;
    ClipboardLog::warn("ClipboardFilesReceiver: %s", error.toUtf8().constData());
    emit failed(error);
}
