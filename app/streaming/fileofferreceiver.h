#pragma once

#include "clipboardsync.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

struct FileOffer
{
    QString id;
    QString name;
    qint64 size = -1;
};

// Downloads files the host offers to this client (clipboard wire kind 4,
// FILE_OFFER). The offer is a small JSON descriptor; the bytes stream over the
// paired mTLS HTTPS connection from /api/v1/file-transfer/<id> into
// "<name>.part", which is renamed once the advertised size has arrived.
// Files are never overwritten, never opened, and are marked as coming from
// the internet (Mark-of-the-Web / quarantine) so the OS treats them as such.
//
// Runs inside the clipboard helper process, on its GUI thread.
class FileOfferReceiver : public QObject
{
    Q_OBJECT

public:
    // Mirrors the host's per-file cap (foundation-sunshine file_transfer_store).
    static constexpr qint64 MAX_FILE_BYTES = 4LL * 1024 * 1024 * 1024;
    static constexpr int MAX_ACTIVE_TRANSFERS = 4;
    static constexpr int MAX_FILE_NAME_CHARS = 180;
    // Aborts a download after this long without any data (not a total cap).
    static constexpr int DEFAULT_IDLE_TIMEOUT_MS = 30000;

    explicit FileOfferReceiver(
        const ClipboardSyncHostContext& hostContext = ClipboardSyncHostContext(),
        QObject* parent = nullptr);
    ~FileOfferReceiver() override;

    void setHostContext(const ClipboardSyncHostContext& hostContext);
    void setDownloadDirectory(const QString& directory);
    QString downloadDirectory() const;
    void setIdleTimeoutMs(int timeoutMs);

    // <Downloads>/Sunshine Transfers, the folder the other Foundation clients use.
    static QString defaultDownloadDirectory();
    static bool parseOffer(const QByteArray& payload, FileOffer& outOffer, QString& outError);
    static QString sanitizeFileName(const QString& name);
    // A path in directory that neither exists nor has a pending ".part".
    static QString uniqueTargetPath(const QString& directory, const QString& fileName);
    static bool markFromInternet(const QString& path);

    int activeTransfers() const;

public slots:
    void handleOffer(const QByteArray& payload);

signals:
    void transferStarted(QString id, QString name, qint64 size);
    void transferFinished(QString id, QString name, QString path);
    void transferFailed(QString id, QString name, QString error);

private:
    struct Transfer
    {
        FileOffer offer;
        QString finalPath;
        QString partPath;
        QFile* file = nullptr;
        qint64 written = 0;
        QString failure;
    };

    QNetworkAccessManager* nam();
    bool buildDownloadUrl(const QString& id, QUrl& outUrl) const;
    void onReadyRead(QNetworkReply* reply);
    void onFinished(QNetworkReply* reply);
    void fail(const FileOffer& offer, const QString& error);

    ClipboardSyncHostContext m_HostContext;
    QString m_DownloadDirectory;
    int m_IdleTimeoutMs = DEFAULT_IDLE_TIMEOUT_MS;
    QNetworkAccessManager* m_Nam = nullptr;
    QHash<QNetworkReply*, Transfer> m_Transfers;
    QSet<QString> m_SeenOfferIds;
};
