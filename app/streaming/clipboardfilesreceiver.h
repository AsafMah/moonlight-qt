#pragma once

#include "clipboardsync.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// Host -> client file copy/paste (copy-then-paste). A FILE_LIST frame from the
// host (wire kind 5, intent "clipboard") names offers bound to this client;
// the files are downloaded into a per-session temporary folder (keeping their
// folders) and then put on the local clipboard as file URLs, so a paste copies
// them from there. Copies live in a persistent app folder (so a paste still
// works after the stream ends) and are swept a day later.
class ClipboardFilesReceiver : public QObject
{
    Q_OBJECT

public:
    static constexpr int MAX_ITEMS = 200;
    static constexpr qint64 MAX_TOTAL_BYTES = 512LL * 1024 * 1024;
    static constexpr int DEFAULT_IDLE_TIMEOUT_MS = 30000;

    struct Item
    {
        QString id;
        QString name;
        QString rel;
        qint64 size = 0;
    };

    explicit ClipboardFilesReceiver(
        const ClipboardSyncHostContext& hostContext = ClipboardSyncHostContext(),
        QObject* parent = nullptr);
    ~ClipboardFilesReceiver() override;

    void setHostContext(const ClipboardSyncHostContext& hostContext);
    void setIdleTimeoutMs(int timeoutMs);
    // Overrides the temporary root (tests).
    void setRootDirectory(const QString& directory);

    // <app local data>/Clipboard Files.
    static QString defaultRootDirectory();
    static constexpr int KEEP_HOURS = 24;

    static bool parseFileList(const QByteArray& payload, QList<Item>& outItems, QString& outError);

    bool isBusy() const;

public slots:
    // A newer list replaces one still downloading.
    void handleFileList(const QByteArray& payload);

signals:
    // Top-level entries ready to put on the clipboard.
    void filesReady(QList<QUrl> urls);
    void failed(QString error);

private:
    QNetworkAccessManager* nam();
    void next();
    void onReadyRead();
    void onFinished();
    void abortCurrent();
    void fail(const QString& error);
    QString rootDirectory();

    ClipboardSyncHostContext m_HostContext;
    QNetworkAccessManager* m_Nam = nullptr;
    int m_IdleTimeoutMs = DEFAULT_IDLE_TIMEOUT_MS;
    QString m_RootOverride;
    bool m_Swept = false;

    QList<Item> m_Items;
    int m_Current = -1;
    QString m_BatchDir;
    QList<QUrl> m_TopLevel;
    QFile* m_File = nullptr;
    qint64 m_Written = 0;
    QString m_Failure;
    QPointer<QNetworkReply> m_Reply;
};
