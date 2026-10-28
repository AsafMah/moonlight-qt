#pragma once

#include <QByteArray>
#include <QString>

namespace ClipboardIpc {

static constexpr int PROTOCOL_VERSION = 1;
static constexpr int MAX_LINE_BYTES = 1024 * 1024;

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
};

// Which clipboard flavors this client syncs, in both directions. A configure
// message without a policy (older main process) means everything enabled.
struct SyncPolicy
{
    bool text = true;
    bool images = true;
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
};

QByteArray encodeConfigure(quint32 sequence, const HostConfig& config);
QByteArray encodeHostFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeLocalFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeReady(quint32 sequence);
QByteArray encodeError(quint32 sequence, const QString& code, const QString& text);
QByteArray encodeStop(quint32 sequence);
QByteArray encodePing(quint32 sequence);
QByteArray encodePong(quint32 sequence);

// Extract one complete bounded line. A partial line returns false without an error.
bool takeLine(QByteArray& buffer, QByteArray& line, QString& error);

bool decodeLine(const QByteArray& line, Message& outMessage, QString& outError);

}
