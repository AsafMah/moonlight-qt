#include "fileofferreceiver.h"

#include "backend/pairedcertificate.h"
#include "clipboardlogging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>

#include <cmath>

#ifdef Q_OS_MACOS
#include <ctime>
#include <sys/xattr.h>
#endif

namespace {

const QString kDownloadPathPrefix = QStringLiteral("/api/v1/file-transfer/");

bool isOfferId(const QString& id)
{
    static const QRegularExpression idRegex(QStringLiteral("^[a-f0-9]{64}$"));
    return idRegex.match(id).hasMatch();
}

bool isForbiddenFileNameChar(QChar c)
{
    const char16_t u = c.unicode();
    if (u < 0x20 || u == 0x7F) {
        return true;
    }
    // Bidirectional overrides can disguise "txt.exe" as "exe.txt".
    if (u == 0x200E || u == 0x200F || (u >= 0x202A && u <= 0x202E) ||
        (u >= 0x2066 && u <= 0x2069)) {
        return true;
    }
    return QStringLiteral("<>:\"/\\|?*").contains(c);
}

void chopTrailingDotsAndSpaces(QString& name)
{
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
}

bool isReservedDeviceName(const QString& name)
{
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|(COM|LPT)[0-9\\x{00B9}\\x{00B2}\\x{00B3}])$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString base = name.section(QLatin1Char('.'), 0, 0).trimmed();
    return reserved.match(base).hasMatch();
}

} // namespace

FileOfferReceiver::FileOfferReceiver(const ClipboardSyncHostContext& hostContext, QObject* parent)
    : QObject(parent), m_HostContext(hostContext)
{
}

FileOfferReceiver::~FileOfferReceiver()
{
    const auto transfers = m_Transfers;
    m_Transfers.clear();
    for (auto it = transfers.cbegin(); it != transfers.cend(); ++it) {
        QNetworkReply* reply = it.key();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        it->file->close();
        QFile::remove(it->partPath);
        delete it->file;
        delete reply;
    }
}

void FileOfferReceiver::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    if (m_HostContext.address == hostContext.address &&
        m_HostContext.httpsPort == hostContext.httpsPort &&
        m_HostContext.serverCertificate == hostContext.serverCertificate &&
        m_HostContext.clientCertificate == hostContext.clientCertificate &&
        m_HostContext.clientPrivateKey == hostContext.clientPrivateKey) {
        return;
    }

    // Transfers in flight belong to the previous paired identity.
    const QList<QNetworkReply*> replies = m_Transfers.keys();
    for (QNetworkReply* reply : replies) {
        auto it = m_Transfers.find(reply);
        if (it != m_Transfers.end()) {
            it->failure = QStringLiteral("host connection changed");
            reply->abort();
        }
    }
    delete m_Nam;
    m_Nam = nullptr;
    m_HostContext = hostContext;
}

void FileOfferReceiver::setDownloadDirectory(const QString& directory)
{
    m_DownloadDirectory = directory;
}

QString FileOfferReceiver::downloadDirectory() const
{
    return m_DownloadDirectory.isEmpty() ? defaultDownloadDirectory() : m_DownloadDirectory;
}

void FileOfferReceiver::setIdleTimeoutMs(int timeoutMs)
{
    m_IdleTimeoutMs = timeoutMs;
}

QString FileOfferReceiver::defaultDownloadDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    return QDir(base).filePath(QStringLiteral("Sunshine Transfers"));
}

bool FileOfferReceiver::parseOffer(const QByteArray& payload, FileOffer& outOffer,
                                   QString& outError)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QStringLiteral("bad JSON");
        return false;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (!type.isEmpty() && type != QStringLiteral("file")) {
        outError = QStringLiteral("unsupported offer type");
        return false;
    }

    const QString id = obj.value(QStringLiteral("id")).toString();
    if (!isOfferId(id)) {
        outError = QStringLiteral("invalid offer id");
        return false;
    }

    const QJsonValue sizeValue = obj.value(QStringLiteral("size"));
    const double size = sizeValue.toDouble(-1);
    if (!sizeValue.isDouble() || !std::isfinite(size) || size < 0 || std::floor(size) != size ||
        size > static_cast<double>(MAX_FILE_BYTES)) {
        outError = QStringLiteral("invalid offer size");
        return false;
    }

    const QString downloadUrl = obj.value(QStringLiteral("download_url")).toString();
    if (!downloadUrl.isEmpty() && downloadUrl != kDownloadPathPrefix + id) {
        outError = QStringLiteral("unexpected download_url");
        return false;
    }

    outOffer.id = id;
    outOffer.name = obj.value(QStringLiteral("name")).toString();
    outOffer.size = static_cast<qint64>(size);
    return true;
}

QString FileOfferReceiver::sanitizeFileName(const QString& name)
{
    QString out = name;
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    out = out.section(QLatin1Char('/'), -1);
    for (QChar& c : out) {
        if (isForbiddenFileNameChar(c)) {
            c = QLatin1Char('_');
        }
    }
    out = out.trimmed();
    chopTrailingDotsAndSpaces(out);
    if (out.isEmpty()) {
        return QStringLiteral("download");
    }

    if (isReservedDeviceName(out)) {
        out.prepend(QLatin1Char('_'));
    }

    if (out.size() > MAX_FILE_NAME_CHARS) {
        const int dot = out.lastIndexOf(QLatin1Char('.'));
        QString suffix = (dot > 0 && out.size() - dot <= 16) ? out.mid(dot) : QString();
        QString stem = out.left(MAX_FILE_NAME_CHARS - suffix.size());
        chopTrailingDotsAndSpaces(stem);
        out = stem.isEmpty() ? QStringLiteral("download") + suffix : stem + suffix;
    }
    return out;
}

QString FileOfferReceiver::uniqueTargetPath(const QString& directory, const QString& fileName)
{
    const QDir dir(directory);
    const QFileInfo info(fileName);
    QString stem = info.completeBaseName();
    QString suffix = info.suffix();
    if (stem.isEmpty()) {
        stem = fileName;
        suffix.clear();
    }

    for (int i = 0; i < 10000; ++i) {
        QString candidate = fileName;
        if (i > 0) {
            candidate = suffix.isEmpty()
                            ? QStringLiteral("%1 (%2)").arg(stem).arg(i)
                            : QStringLiteral("%1 (%2).%3").arg(stem).arg(i).arg(suffix);
        }
        const QString path = dir.filePath(candidate);
        if (!QFileInfo::exists(path) && !QFileInfo::exists(path + QStringLiteral(".part"))) {
            return path;
        }
    }
    return QString();
}

bool FileOfferReceiver::markFromInternet(const QString& path)
{
#if defined(Q_OS_WIN)
    QFile zone(path + QStringLiteral(":Zone.Identifier"));
    if (!zone.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray data("[ZoneTransfer]\r\nZoneId=3\r\n");
    return zone.write(data) == data.size();
#elif defined(Q_OS_MACOS)
    const QByteArray value =
        QStringLiteral("0081;%1;Moonlight;")
            .arg(static_cast<qulonglong>(std::time(nullptr)), 8, 16, QLatin1Char('0'))
            .toUtf8();
    return setxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", value.constData(),
                    static_cast<size_t>(value.size()), 0, 0) == 0;
#else
    Q_UNUSED(path);
    return true;
#endif
}

int FileOfferReceiver::activeTransfers() const
{
    return m_Transfers.size();
}

void FileOfferReceiver::handleOffer(const QByteArray& payload)
{
    FileOffer offer;
    QString error;
    if (!parseOffer(payload, offer, error)) {
        ClipboardLog::warn("FileOfferReceiver: dropping host file offer (%s)",
                           error.toUtf8().constData());
        return;
    }
    // Hosts may repeat an offer; each id is saved at most once per session.
    if (m_SeenOfferIds.contains(offer.id)) {
        return;
    }
    m_SeenOfferIds.insert(offer.id);

    const QString fileName = sanitizeFileName(offer.name);
    offer.name = fileName;

    if (m_Transfers.size() >= MAX_ACTIVE_TRANSFERS) {
        fail(offer, QStringLiteral("too many transfers in progress"));
        return;
    }

    QUrl url;
    if (!buildDownloadUrl(offer.id, url)) {
        fail(offer, QStringLiteral("no host connection"));
        return;
    }

    const QString directory = downloadDirectory();
    if (!QDir().mkpath(directory)) {
        fail(offer, QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(directory)));
        return;
    }
    const QStorageInfo storage(directory);
    if (storage.isValid() && storage.bytesAvailable() >= 0 &&
        storage.bytesAvailable() < offer.size) {
        fail(offer, QStringLiteral("not enough disk space"));
        return;
    }

    Transfer transfer;
    transfer.offer = offer;
    transfer.finalPath = uniqueTargetPath(directory, fileName);
    if (transfer.finalPath.isEmpty()) {
        fail(offer, QStringLiteral("no free file name"));
        return;
    }
    transfer.partPath = transfer.finalPath + QStringLiteral(".part");
    transfer.file = new QFile(transfer.partPath);
    if (!transfer.file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const QString reason = transfer.file->errorString();
        delete transfer.file;
        fail(offer, reason);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    request.setSslConfiguration(sslConfig);
    request.setTransferTimeout(m_IdleTimeoutMs);

    QNetworkReply* reply = nam()->get(request);
    m_Transfers.insert(reply, transfer);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() { onReadyRead(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onFinished(reply); });

    ClipboardLog::info("FileOfferReceiver: receiving %s (%lld bytes)",
                       fileName.toUtf8().constData(), static_cast<long long>(offer.size));
    emit transferStarted(offer.id, fileName, offer.size);
}

QNetworkAccessManager* FileOfferReceiver::nam()
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

bool FileOfferReceiver::buildDownloadUrl(const QString& id, QUrl& outUrl) const
{
    if (m_HostContext.address.isEmpty() || m_HostContext.httpsPort == 0 ||
        m_HostContext.serverCertificate.isNull()) {
        return false;
    }
    outUrl = QUrl();
    outUrl.setScheme(QStringLiteral("https"));
    outUrl.setHost(m_HostContext.address);
    outUrl.setPort(m_HostContext.httpsPort);
    outUrl.setPath(kDownloadPathPrefix + id);
    return outUrl.isValid();
}

void FileOfferReceiver::onReadyRead(QNetworkReply* reply)
{
    auto it = m_Transfers.find(reply);
    if (it == m_Transfers.end() || !it->failure.isEmpty()) {
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        it->failure = QStringLiteral("host answered HTTP %1").arg(status);
        reply->abort();
        return;
    }

    const QByteArray chunk = reply->readAll();
    if (it->written + chunk.size() > it->offer.size) {
        it->failure = QStringLiteral("host sent more data than offered");
        reply->abort();
        return;
    }
    if (it->file->write(chunk) != chunk.size()) {
        it->failure = it->file->errorString();
        reply->abort();
        return;
    }
    it->written += chunk.size();
}

void FileOfferReceiver::onFinished(QNetworkReply* reply)
{
    if (reply->bytesAvailable() > 0) {
        onReadyRead(reply);
    }
    auto it = m_Transfers.find(reply);
    if (it == m_Transfers.end()) {
        return;
    }
    Transfer transfer = *it;
    m_Transfers.erase(it);
    reply->deleteLater();

    QString failure = transfer.failure;
    if (failure.isEmpty() && reply->error() != QNetworkReply::NoError) {
        failure = reply->errorString();
    }
    if (failure.isEmpty() &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        failure = QStringLiteral("host answered HTTP %1")
                      .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    }
    if (failure.isEmpty() && transfer.written != transfer.offer.size) {
        failure = QStringLiteral("incomplete download (%1 of %2 bytes)")
                      .arg(transfer.written)
                      .arg(transfer.offer.size);
    }
    if (failure.isEmpty() && !transfer.file->flush()) {
        failure = transfer.file->errorString();
    }
    transfer.file->close();
    delete transfer.file;

    if (failure.isEmpty() && QFileInfo::exists(transfer.finalPath)) {
        // Something claimed the name while downloading; never overwrite it.
        transfer.finalPath = uniqueTargetPath(QFileInfo(transfer.finalPath).absolutePath(),
                                              QFileInfo(transfer.finalPath).fileName());
    }
    if (failure.isEmpty() &&
        (transfer.finalPath.isEmpty() || !QFile::rename(transfer.partPath, transfer.finalPath))) {
        failure = QStringLiteral("cannot finish writing the file");
    }
    if (!failure.isEmpty()) {
        QFile::remove(transfer.partPath);
        fail(transfer.offer, failure);
        return;
    }

    if (!markFromInternet(transfer.finalPath)) {
        ClipboardLog::warn("FileOfferReceiver: could not mark %s as downloaded",
                           transfer.finalPath.toUtf8().constData());
    }
    ClipboardLog::info("FileOfferReceiver: saved %s (%lld bytes)",
                       transfer.finalPath.toUtf8().constData(),
                       static_cast<long long>(transfer.written));
    emit transferFinished(transfer.offer.id, transfer.offer.name, transfer.finalPath);
}

void FileOfferReceiver::fail(const FileOffer& offer, const QString& error)
{
    ClipboardLog::warn("FileOfferReceiver: %s failed: %s", offer.name.toUtf8().constData(),
                       error.toUtf8().constData());
    emit transferFailed(offer.id, offer.name, error);
}
