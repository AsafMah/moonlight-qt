#include "clipboardhelperthreading.h"
#include "streaming/clipboardsync.h"
#include "streaming/clipboardipc.h"
#include "streaming/fileofferreceiver.h"
#include "streaming/fileuploader.h"
#include "streaming/clipboardfilesreceiver.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef Q_OS_MACOS
extern "C" void ClipboardHelperSetBackgroundActivationPolicy();
#endif

class StdinReaderThread : public QThread
{
    Q_OBJECT

signals:
    void lineReceived(QByteArray line);

protected:
    void run() override
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            emit lineReceived(QByteArray(line.data(), static_cast<int>(line.size())));
        }
        emit lineReceived(ClipboardIpc::encodeStop(0));
    }
};

// Single owner of stdout. Every protocol line goes through this thread so a
// wedged GUI thread (a clipboard write blocked on pasteboard contention)
// cannot stall host-bound traffic: outbound frames are enqueued from the
// snapshot thread directly, never crossing the GUI event loop. The queue is
// bounded by total bytes, symmetric with the session's stdin backlog cap.
class ProtocolWriter
{
public:
    explicit ProtocolWriter()
    {
        m_Thread = std::thread([this] { run(); });
    }
    ~ProtocolWriter() { stopAndDrain(); }

    // Thread-safe. Returns false when the line was dropped: the backlog cap
    // was hit, or the writer already failed (queued lines would never be
    // flushed, so callers must not count on delivery).
    bool write(QByteArray line)
    {
        line += '\n';
        bool ok = true;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Failed) {
                ok = false;
            } else if (m_TotalBytes + line.size() > MAX_PENDING_BYTES) {
                m_Failed = true;
                ok = false;
            } else {
                m_Queue.push_back(std::move(line));
                m_TotalBytes += m_Queue.back().size();
            }
        }
        m_Condition.notify_one();
        return ok;
    }

    bool failed() const { return m_Failed; }

    void stopAndDrain()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Stopping) {
                return;
            }
            m_Stopping = true;
        }
        m_Condition.notify_all();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

private:
    static constexpr size_t MAX_PENDING_BYTES = 4 * 1024 * 1024;

    void run()
    {
        for (;;) {
            QByteArray line;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Condition.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });
                if (!m_Queue.empty()) {
                    line = std::move(m_Queue.front());
                    m_Queue.pop_front();
                    m_TotalBytes -= line.size();
                } else if (m_Stopping) {
                    return;
                }
            }
            size_t written = fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
            // One flush, checked: a first unchecked flush could discard the
            // buffered line while a second flush reports success (UCRT clears
            // the buffer even when the underlying write failed).
            const bool flushed = fflush(stdout) == 0;
            if (written != static_cast<size_t>(line.size()) || !flushed) {
                // Reader is gone; nothing else can be delivered.
                m_Failed = true;
                QCoreApplication::quit();
                return;
            }
        }
    }

    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    std::deque<QByteArray> m_Queue;
    size_t m_TotalBytes = 0;
    bool m_Stopping = false;
    std::atomic<bool> m_Failed{ false };
};

class ClipboardHelperController : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardHelperController(QObject* parent = nullptr)
        : QObject(parent)
    {
        // The engine's dedicated thread (macOS only; see
        // clipboardhelperthreading.h). There the GUI main thread keeps only
        // the event loop and never touches the clipboard.
        m_WorkerThread = new QThread(this);
        m_WorkerThread->setObjectName("clipboard-engine");
        m_WorkerThread->start();
    }
    ~ClipboardHelperController() override
    {
        m_WorkerThread->quit();
        m_WorkerThread->wait(2000);
    }

    QThread* workerThread() const { return m_WorkerThread; }

    void setWriter(ProtocolWriter* writer) { m_Writer = writer; }

public slots:
    void handleLine(const QByteArray& line)
    {
        ClipboardIpc::Message message;
        QString error;
        if (!ClipboardIpc::decodeLine(line, message, error)) {
            writeProtocolLine(ClipboardIpc::encodeError(0, QStringLiteral("bad-message"), error));
            return;
        }

        if (message.type == ClipboardIpc::MessageType::Stop) {
            QCoreApplication::quit();
            return;
        }
        if (message.type == ClipboardIpc::MessageType::Ping) {
            writeProtocolLine(ClipboardIpc::encodePong(message.sequence));
            return;
        }

        if (message.type == ClipboardIpc::MessageType::Configure) {
            handleConfig(message.sequence, message.config);
            return;
        }

        if (message.type == ClipboardIpc::MessageType::UploadFiles) {
            if (m_Uploader == nullptr) {
                writeProtocolLine(
                    ClipboardIpc::encodeError(message.sequence, QStringLiteral("not-ready"),
                                              QStringLiteral("clipboard sync is not configured")));
                return;
            }
            UploadRequest request;
            request.paths = message.upload.paths;
            request.intent = message.upload.intent;
            request.x = message.upload.x;
            request.y = message.upload.y;
            m_Uploader->start(request);
            return;
        }

        if (message.type == ClipboardIpc::MessageType::CancelTransfers) {
            if (m_Uploader != nullptr) {
                m_Uploader->cancel();
            }
            return;
        }

        if (message.type == ClipboardIpc::MessageType::HostFrame) {
            if (m_ClipboardEngine == nullptr) {
                writeProtocolLine(ClipboardIpc::encodeError(message.sequence,
                                                            QStringLiteral("not-ready"),
                                                            QStringLiteral("clipboard sync is not configured")));
                return;
            }
            m_ClipboardEngine->handleIncomingFrame(message.frame.constData(), message.frame.size());
            return;
        }

        writeProtocolLine(ClipboardIpc::encodeError(message.sequence,
                                                    QStringLiteral("unexpected-message"),
                                                    QStringLiteral("message type is not valid for helper input")));
    }

private:
    void handleConfig(quint32 sequence, const ClipboardIpc::HostConfig& config)
    {
        if (config.address.isEmpty() || config.httpsPort == 0 ||
                config.serverCertPem.isEmpty() ||
                config.clientCertPem.isEmpty() ||
                config.clientKeyPem.isEmpty()) {
            writeProtocolLine(ClipboardIpc::encodeError(sequence,
                                                        QStringLiteral("bad-config"),
                                                        QStringLiteral("missing host address, port, or certificate data")));
            return;
        }

        ClipboardSyncHostContext context;
        context.address = config.address;
        context.httpsPort = config.httpsPort;
        context.serverCertificate = QSslCertificate(config.serverCertPem);
        context.clientCertificate = QSslCertificate(config.clientCertPem);
        context.clientPrivateKey = QSslKey(config.clientKeyPem, QSsl::Rsa);
        if (context.serverCertificate.isNull() ||
                context.clientCertificate.isNull() ||
                context.clientPrivateKey.isNull()) {
            writeProtocolLine(ClipboardIpc::encodeError(sequence,
                                                        QStringLiteral("bad-config"),
                                                        QStringLiteral("certificate or private key data is unreadable")));
            return;
        }

        ClipboardSyncPolicy policy;
        policy.text = config.policy.text;
        policy.images = config.policy.images;
        policy.fileOffers = config.policy.fileOffers;
        policy.files = config.policy.files;

        if (m_ClipboardEngine == nullptr) {
            // On macOS the engine lives on its own thread for its whole life:
            // inbound clipboard writes happen there, so pasteboard contention
            // can never wedge the GUI thread. Elsewhere Qt's clipboard only
            // works on the GUI thread (see clipboardhelperthreading.h).
            auto* engine = new ClipboardSync(context);
            engine->setPolicy(policy);
            if (ClipboardHelperThreading::engineUsesWorkerThread()) {
                adoptEngine(engine, m_WorkerThread);
                connect(m_WorkerThread, &QThread::finished, engine, &QObject::deleteLater);
            } else {
                // Windows/Linux: Qt's clipboard only works on the GUI thread.
                engine->setParent(this);
                m_ClipboardEngine = engine;
            }
            // Outbound frames are produced on the engine thread and enqueued
            // onto the writer directly (the writer owns stdout).
            connect(engine, &ClipboardSync::outboundFrame, this,
                    &ClipboardHelperController::sendLocalFrame, Qt::DirectConnection);
            // File transfers stay on the GUI thread, so a clipboard write blocked
            // on the engine thread can't stall a download. Offers are decoded on
            // the engine thread and queued here.
            m_FileOffers = new FileOfferReceiver(context, this);
            // Uploads run on the GUI thread too; the finished file list is handed
            // to the engine thread, which owns the outbound frame path.
            m_Uploader = new FileUploader(context, this);
            connect(m_Uploader, &FileUploader::fileListReady, this, [this](QByteArray payload) {
                postToEngine(
                    [engine = m_ClipboardEngine, payload] { engine->sendFileList(payload); });
            });
            connect(m_Uploader, &FileUploader::started, this,
                    [this](QString id, int count, qint64 total) {
                        sendUploadEvent(ClipboardIpc::TransferState::Started, id, count, 0, total);
                    });
            connect(m_Uploader, &FileUploader::progress, this,
                    [this](QString id, qint64 sent, qint64 total) {
                        sendUploadEvent(ClipboardIpc::TransferState::Progress, id, 0, sent, total);
                    });
            connect(m_Uploader, &FileUploader::finished, this,
                    [this](QString id, int count, qint64 total) {
                        sendUploadEvent(ClipboardIpc::TransferState::Finished, id, count, total,
                                        total);
                    });
            connect(m_Uploader, &FileUploader::failed, this, [this](QString id, QString error) {
                sendUploadEvent(ClipboardIpc::TransferState::Failed, id, 0, 0, -1, error);
            });
            connect(m_Uploader, &FileUploader::superseded, this, [this](QString id) {
                // Empty error: replaced by a newer copy, nothing to report.
                sendUploadEvent(ClipboardIpc::TransferState::Failed, id, 0, 0, -1, QString());
            });
            // File copy/paste. Copies are detected on the engine thread and
            // uploaded from here; host file lists download here and are applied
            // back on the engine thread, which owns the clipboard.
            connect(
                engine, &ClipboardSync::localFilesCopied, this,
                [this](QStringList paths) {
                    UploadRequest request;
                    request.paths = paths;
                    request.intent = QStringLiteral("clipboard");
                    request.maxTotalBytes = ClipboardFilesReceiver::MAX_TOTAL_BYTES;
                    m_Uploader->start(request);
                },
                Qt::QueuedConnection);
            m_FilesReceiver = new ClipboardFilesReceiver(context, this);
            connect(
                engine, &ClipboardSync::fileListReceived, this,
                [this](QByteArray payload, quint64 revision) {
                    // A clipboard change after this revision beats the download.
                    m_FilesRevision = revision;
                    m_FilesReceiver->handleFileList(payload);
                },
                Qt::QueuedConnection);
            connect(m_FilesReceiver, &ClipboardFilesReceiver::filesReady, this,
                    [this](QList<QUrl> urls) {
                        postToEngine(
                            [this, engine = m_ClipboardEngine, urls, revision = m_FilesRevision] {
                                if (engine->applyLocalFiles(urls, revision)) {
                                    sendCopiedFilesEvent(ClipboardIpc::TransferState::Finished,
                                                         urls.size());
                                }
                            });
                    });
            connect(m_FilesReceiver, &ClipboardFilesReceiver::failed, this, [this](QString error) {
                sendCopiedFilesEvent(ClipboardIpc::TransferState::Failed, 0, error);
            });
            connect(engine, &ClipboardSync::fileOfferReceived, m_FileOffers,
                    &FileOfferReceiver::handleOffer, Qt::QueuedConnection);
            connect(m_FileOffers, &FileOfferReceiver::transferStarted, this,
                    [this](QString id, QString name, qint64 size) {
                        sendTransferEvent(ClipboardIpc::TransferState::Started, id, name, size);
                    });
            connect(m_FileOffers, &FileOfferReceiver::transferFinished, this,
                    [this](QString id, QString name, QString path) {
                        sendTransferEvent(ClipboardIpc::TransferState::Finished, id, name, -1,
                                          path);
                    });
            connect(m_FileOffers, &FileOfferReceiver::transferFailed, this,
                    [this](QString id, QString name, QString error) {
                        sendTransferEvent(ClipboardIpc::TransferState::Failed, id, name, -1,
                                          QString(), error);
                    });
            postToEngine([engine] { engine->start(); });
        }
        else {
            postToEngine([engine = m_ClipboardEngine, policy, context] {
                engine->setPolicy(policy);
                engine->setHostContext(context);
            });
            m_FileOffers->setHostContext(context);
            m_Uploader->setHostContext(context);
            m_FilesReceiver->setHostContext(context);
        }

        writeProtocolLine(ClipboardIpc::encodeReady(sequence));
    }

    void sendLocalFrame(const QByteArray& frame)
    {
        writeProtocolLine(ClipboardIpc::encodeLocalFrame(m_NextSequence++, frame));
    }

    // Host -> client file copy; the main process shows it as a paste notice.
    // Thread-safe (writer + atomic sequence): also called on the engine thread.
    void sendCopiedFilesEvent(ClipboardIpc::TransferState state, int count,
                              const QString& error = QString())
    {
        ClipboardIpc::TransferEvent event;
        event.state = state;
        event.direction = ClipboardIpc::TransferDirection::Download;
        event.id = QLatin1String(ClipboardIpc::COPIED_FILES_ID);
        event.count = count;
        event.error = error;
        writeProtocolLine(ClipboardIpc::encodeTransferEvent(m_NextSequence++, event));
    }

    void sendUploadEvent(ClipboardIpc::TransferState state, const QString& id, int count,
                         qint64 bytes, qint64 size, const QString& error = QString())
    {
        ClipboardIpc::TransferEvent event;
        event.state = state;
        event.direction = ClipboardIpc::TransferDirection::Upload;
        event.id = id;
        event.count = count;
        event.bytes = bytes;
        event.size = size;
        event.error = error;
        writeProtocolLine(ClipboardIpc::encodeTransferEvent(m_NextSequence++, event));
    }

    void sendTransferEvent(ClipboardIpc::TransferState state, const QString& id,
                           const QString& name, qint64 size, const QString& path = QString(),
                           const QString& error = QString())
    {
        ClipboardIpc::TransferEvent event;
        event.state = state;
        event.id = id;
        event.name = name;
        event.size = size;
        event.path = path;
        event.error = error;
        writeProtocolLine(ClipboardIpc::encodeTransferEvent(m_NextSequence++, event));
    }

    void writeProtocolLine(const QByteArray& line)
    {
        if (m_Writer != nullptr && !m_Writer->write(line)) {
            QCoreApplication::quit();
        }
    }

    // Move the engine onto its own thread (no parent, or moveToThread
    // refuses). All engine methods are then invoked through queued functors
    // targeting the engine, so they execute on that thread — including the
    // clipboard writes whose blocking is what this refactor defends against.
    void adoptEngine(ClipboardSync* engine, QThread* worker)
    {
        engine->moveToThread(worker);
        m_ClipboardEngine = engine;
    }

    template <typename Fn> void postToEngine(Fn&& fn)
    {
        if (m_ClipboardEngine == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            m_ClipboardEngine,
            [engine = QPointer<ClipboardSync>(m_ClipboardEngine),
             fn = std::forward<Fn>(fn)]() mutable {
                if (engine != nullptr) {
                    fn();
                }
            },
            Qt::QueuedConnection);
    }

    ProtocolWriter* m_Writer = nullptr;
    QThread* m_WorkerThread = nullptr;
    ClipboardSync* m_ClipboardEngine = nullptr;
    FileOfferReceiver* m_FileOffers = nullptr;
    FileUploader* m_Uploader = nullptr;
    ClipboardFilesReceiver* m_FilesReceiver = nullptr;
    quint64 m_FilesRevision = 0;
    // Touched from snapshot threads (inline frames) and the GUI thread (REF
    // callbacks and file transfer events), so the increment must be atomic.
    std::atomic<quint32> m_NextSequence{ 1 };
};

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
#ifdef Q_OS_MACOS
    ClipboardHelperSetBackgroundActivationPolicy();
#endif
    QCoreApplication::setApplicationName(QStringLiteral("Moonlight Clipboard Helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("Moonlight Game Streaming Project"));

    ProtocolWriter writer;
    ClipboardHelperController controller;
    controller.setWriter(&writer);
    StdinReaderThread stdinThread;
    // Bound the reader to one GUI delivery at a time. Otherwise a blocked
    // clipboard provider lets the worker enqueue an unbounded number of frames.
    QObject::connect(&stdinThread, &StdinReaderThread::lineReceived, &controller,
                     &ClipboardHelperController::handleLine, Qt::BlockingQueuedConnection);

    stdinThread.start();
    int rc = app.exec();
    // Flush any protocol lines still queued, stop the writer, then wind the
    // engine thread down (the queued engine stop is best-effort at exit).
    writer.stopAndDrain();
    if (controller.workerThread() != nullptr) {
        controller.workerThread()->quit();
        if (!controller.workerThread()->wait(2000)) {
            // The engine thread is stuck in a clipboard call (the stalled-
            // pasteboard case). Unwinding would destroy a running QThread
            // and abort via qFatal; exit immediately instead — the OS
            // reclaims the stuck thread and the writer has already drained.
            std::fflush(nullptr);
            std::exit(rc);
        }
    }
    if (!stdinThread.wait(1000)) {
        stdinThread.terminate();
        stdinThread.wait();
    }
    // A writer failure means protocol output was lost; surface it in the
    // exit status so the session log shows an abnormal helper termination.
    if (rc == 0 && writer.failed()) {
        rc = 3;
    }
    return rc;
}

#include "main.moc"
