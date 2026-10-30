#pragma once

#include "clipboardipc.h"
#include "clipboardsync.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <optional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

struct UploadItem
{
    QString localPath;
    QString name;
    // Relative folder inside the drop ("" or "Saves/slot1"), '/' separated.
    QString rel;
    qint64 size = 0;
};

struct UploadRequest
{
    QStringList paths;
    QString intent = QStringLiteral("drop");
    std::optional<double> x;
    std::optional<double> y;
    // Copies are uploaded eagerly, so they get a lower cap than drops.
    qint64 maxTotalBytes = 16LL * 1024 * 1024 * 1024;
};

// Sends local files to a Foundation Sunshine host (client-to-host upload API,
// /api/v1/file-transfer/batches|uploads over the paired mTLS connection) and,
// once the host has committed the batch, produces the FILE_LIST payload
// (clipboard wire kind 5) that tells the host agent to pick the files up.
// One batch at a time; runs in the clipboard helper process on its GUI thread.
class FileUploader : public QObject
{
    Q_OBJECT

public:
    // Mirror the host's limits (foundation-sunshine file_upload_store.h).
    static constexpr int MAX_FILES = ClipboardIpc::MAX_UPLOAD_PATHS;
    static constexpr qint64 MAX_FILE_BYTES = 4LL * 1024 * 1024 * 1024;
    static constexpr qint64 MAX_TOTAL_BYTES = 16LL * 1024 * 1024 * 1024;
    static constexpr int MAX_DEPTH = 8;
    static constexpr qint64 CHUNK_BYTES = 4LL * 1024 * 1024;
    static constexpr int DEFAULT_IDLE_TIMEOUT_MS = 30000;

    explicit FileUploader(const ClipboardSyncHostContext& hostContext = ClipboardSyncHostContext(),
                          QObject* parent = nullptr);
    ~FileUploader() override;

    void setHostContext(const ClipboardSyncHostContext& hostContext);
    void setIdleTimeoutMs(int timeoutMs);
    void setChunkBytes(qint64 chunkBytes);

    // Expands dropped files and folders into upload items. Folders keep their
    // structure (rel); links and junctions are skipped, never followed.
    static bool planUpload(const QStringList& paths, QList<UploadItem>& outItems,
                           QString& outError);

    bool isBusy() const;

public slots:
    // Returns immediately; outcome is reported via the signals below.
    void start(const UploadRequest& request);
    void cancel();

signals:
    void started(QString id, int count, qint64 totalBytes);
    void progress(QString id, qint64 sentBytes, qint64 totalBytes);
    void finished(QString id, int count, qint64 totalBytes);
    void failed(QString id, QString error);
    // A copy upload was dropped in favor of a newer copy (no error to show).
    void superseded(QString id);
    // JSON payload for a KIND_FILE_LIST frame.
    void fileListReady(QByteArray payload);

private:
    enum class Stage
    {
        Idle,
        OpenBatch,
        CreateUpload,
        SendChunk,
        Commit,
    };

    QNetworkAccessManager* nam();
    bool buildUrl(const QString& path, const QString& query, QUrl& outUrl) const;
    QNetworkReply* send(const QByteArray& verb, const QString& path, const QString& query,
                        const QByteArray& body, const QByteArray& contentType);
    void openBatch();
    void createUpload();
    void sendChunk();
    void commit();
    void onReply(QNetworkReply* reply);
    // Returns the batch DELETE request still in flight, if any.
    QNetworkReply* fail(const QString& error);
    // Aborts the batch locally and on the host; returns the host DELETE request.
    QNetworkReply* stopCurrent();
    void startPendingCopy();
    void finishBatch();
    QNetworkReply* abortRemote();
    static QString describeHttpError(int status, const QByteArray& body);

    ClipboardSyncHostContext m_HostContext;
    QNetworkAccessManager* m_Nam = nullptr;
    int m_IdleTimeoutMs = DEFAULT_IDLE_TIMEOUT_MS;
    qint64 m_ChunkBytes = CHUNK_BYTES;

    Stage m_Stage = Stage::Idle;
    QString m_Id;
    UploadRequest m_Request;
    QList<UploadItem> m_Items;
    qint64 m_TotalBytes = 0;
    qint64 m_SentBytes = 0;
    int m_Current = -1;
    QString m_BatchId;
    QString m_UploadId;
    qint64 m_Offset = 0;
    QFile* m_File = nullptr;
    QPointer<QNetworkReply> m_Reply;
    std::optional<UploadRequest> m_PendingCopy;
};
