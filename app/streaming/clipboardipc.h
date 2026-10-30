#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace ClipboardIpc {

static constexpr int PROTOCOL_VERSION = 1;
static constexpr int MAX_LINE_BYTES = 1024 * 1024;
// Paths per uploadFiles message; matches FileUploader::MAX_FILES.
static constexpr int MAX_UPLOAD_PATHS = 1000;

enum class MessageType
{
    Unknown,
    Configure,
    HostFrame,
    LocalFrame,
    Ready,
    Error,
    Stop,
    Ping,
    Pong,
    TransferEvent,
    UploadFiles,
    CancelTransfers,
};

// Which clipboard flavors this client syncs, in both directions. A configure
// message without a policy (older main process) means everything enabled.
struct SyncPolicy
{
    bool text = true;
    bool images = true;
    bool fileOffers = true;
};

enum class TransferState
{
    Started,
    Progress,
    Finished,
    Failed,
};

enum class TransferDirection
{
    Download, // host file offer -> this device
    Upload,   // dropped/copied files -> host
};

// Helper -> main: transfer progress, shown as overlay toasts.
struct TransferEvent
{
    TransferState state = TransferState::Started;
    TransferDirection direction = TransferDirection::Download;
    QString id;
    QString name;
    int count = 1;
    qint64 bytes = 0;
    qint64 size = -1;
    QString path;
    QString error;
};

// Main -> helper: send local files to the host.
struct UploadFiles
{
    QStringList paths;
    QString intent = QStringLiteral("drop");
    std::optional<double> x;
    std::optional<double> y;
};

struct HostConfig {
    SyncPolicy policy;
    QString address;
    quint16 httpsPort = 0;
    QByteArray serverCertPem;
    QByteArray clientCertPem;
    QByteArray clientKeyPem;
};

struct Message {
    MessageType type = MessageType::Unknown;
    quint32 sequence = 0;
    HostConfig config;
    QByteArray frame;
    QString code;
    QString text;
    ClipboardIpc::TransferEvent transfer;
    ClipboardIpc::UploadFiles upload;
};

QByteArray encodeConfigure(quint32 sequence, const HostConfig& config);
QByteArray encodeHostFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeLocalFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeReady(quint32 sequence);
QByteArray encodeError(quint32 sequence, const QString& code, const QString& text);
QByteArray encodeStop(quint32 sequence);
QByteArray encodePing(quint32 sequence);
QByteArray encodePong(quint32 sequence);
QByteArray encodeTransferEvent(quint32 sequence, const TransferEvent& event);
QByteArray encodeUploadFiles(quint32 sequence, const UploadFiles& upload);
QByteArray encodeCancelTransfers(quint32 sequence);

// Extract one complete bounded line. A partial line returns false without an error.
bool takeLine(QByteArray& buffer, QByteArray& line, QString& error);

bool decodeLine(const QByteArray& line, Message& outMessage, QString& outError);

}
