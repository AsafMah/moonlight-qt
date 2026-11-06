#include "streaming/displaygroupipc.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <atomic>

namespace DisplayGroup {

namespace {

class ProcessMemberLauncher : public MemberLauncher
{
public:
    ~ProcessMemberLauncher() override { terminateAll(0); }

    void launch(int head, const QStringList& arguments) override
    {
        auto* process = new QProcess();
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments(arguments);
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        QObject::connect(process, &QProcess::finished, process,
                         [head](int exitCode, QProcess::ExitStatus status) {
                             qInfo()
                                 << "Display group member for head" << head << "exited:" << exitCode
                                 << (status == QProcess::CrashExit ? "(crash)" : "");
                         });
        process->start();
        if (!process->waitForStarted(5000)) {
            qWarning() << "Failed to start display group member for head" << head << ":"
                       << process->errorString();
        } else {
            qInfo() << "Started display group member for head" << head;
        }
        m_Processes.append(process);
    }

    void terminateAll(int graceMs) override
    {
        QElapsedTimer timer;
        timer.start();
        for (QProcess* process : std::as_const(m_Processes)) {
            if (process->state() != QProcess::NotRunning) {
                const int remaining = qMax(0, graceMs - int(timer.elapsed()));
                if (!process->waitForFinished(remaining)) {
                    qWarning() << "Killing display group member that did not exit";
                    process->kill();
                    process->waitForFinished(1000);
                }
            }
            delete process;
        }
        m_Processes.clear();
    }

private:
    QVector<QProcess*> m_Processes;
};

} // namespace

std::unique_ptr<MemberLauncher> createProcessMemberLauncher()
{
    return std::make_unique<ProcessMemberLauncher>();
}

class LeaderHubWorker : public QObject
{
public:
    LeaderHubWorker(KeyRelay::Sender keySender, LeaderHub::TextSender textSender,
                    std::unique_ptr<MemberLauncher> launcher)
        : m_Relay(std::move(keySender)), m_TextSender(std::move(textSender)),
          m_Launcher(std::move(launcher))
    {
    }

    bool listen(const QString& name)
    {
        m_Server = new QLocalServer(this);
        m_Server->setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(name);
        if (!m_Server->listen(name)) {
            qWarning() << "Display group hub failed to listen:" << m_Server->errorString();
            return false;
        }
        connect(m_Server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = m_Server->nextPendingConnection()) {
                accept(socket);
            }
        });
        return true;
    }

    void launch(const QVector<QPair<int, QStringList>>& members)
    {
        if (m_ShuttingDown || !m_Launcher) {
            return;
        }
        for (const auto& member : members) {
            if (m_Launched.contains(member.first)) {
                continue;
            }
            m_Launched.insert(member.first);
            launchedCount = int(m_Launched.size());
            m_Launcher->launch(member.first, member.second);
        }
    }

    void shutdown(int graceMs)
    {
        m_ShuttingDown = true;
        const QList<QLocalSocket*> sockets = m_Heads.keys();
        IpcMessage quit;
        quit.type = IpcMessage::Quit;
        const QByteArray quitLine = encodeIpc(quit);
        for (QLocalSocket* socket : sockets) {
            const int head = m_Heads.value(socket, -1);
            if (head >= 0) {
                socket->write(quitLine);
                socket->flush();
                socket->waitForBytesWritten(200);
                m_Relay.releaseMember(head);
            }
            socket->disconnect(this);
            socket->disconnectFromServer();
            socket->deleteLater();
        }
        m_Heads.clear();
        connectedCount = 0;
        if (m_Launcher) {
            m_Launcher->terminateAll(graceMs);
            m_Launcher.reset();
        }
        if (m_Server != nullptr) {
            m_Server->close();
        }
    }

    std::atomic<int> connectedCount{ 0 };
    std::atomic<int> launchedCount{ 0 };

private:
    void accept(QLocalSocket* socket)
    {
        if (m_ShuttingDown) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        m_Heads.insert(socket, -1);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readLines(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { drop(socket); });
    }

    void readLines(QLocalSocket* socket)
    {
        while (socket->canReadLine()) {
            const IpcMessage message = decodeIpcLine(socket->readLine().trimmed());
            int head = m_Heads.value(socket, -1);
            switch (message.type) {
            case IpcMessage::Hello:
                if (head < 0 && message.version == kIpcProtocolVersion && message.head > 0) {
                    // A restarted member for the same head replaces the old link.
                    QList<QLocalSocket*> replaced;
                    for (auto it = m_Heads.begin(); it != m_Heads.end(); ++it) {
                        if (it.value() == message.head) {
                            it.value() = -1;
                            replaced.append(it.key());
                        }
                    }
                    if (!replaced.isEmpty()) {
                        m_Relay.releaseMember(message.head);
                    }
                    m_Heads[socket] = message.head;
                    updateConnectedCount();
                    for (QLocalSocket* old : replaced) {
                        old->abort();
                    }
                    qInfo() << "Display group member joined for head" << message.head;
                }
                break;
            case IpcMessage::Key:
                if (head > 0) {
                    m_Relay.forward(head, message.code, message.action, message.modifiers,
                                    message.flags);
                }
                break;
            case IpcMessage::Text:
                if (head > 0 && m_TextSender) {
                    m_TextSender(message.text);
                }
                break;
            default:
                break;
            }
        }
    }

    void drop(QLocalSocket* socket)
    {
        const int head = m_Heads.take(socket);
        if (head > 0) {
            qInfo() << "Display group member left for head" << head;
            m_Relay.releaseMember(head);
        }
        updateConnectedCount();
        socket->deleteLater();
    }

    void updateConnectedCount()
    {
        int count = 0;
        for (int head : std::as_const(m_Heads)) {
            count += head > 0 ? 1 : 0;
        }
        connectedCount = count;
    }

    QLocalServer* m_Server = nullptr;
    QHash<QLocalSocket*, int> m_Heads;
    QSet<int> m_Launched;
    KeyRelay m_Relay;
    LeaderHub::TextSender m_TextSender;
    std::unique_ptr<MemberLauncher> m_Launcher;
    bool m_ShuttingDown = false;
};

LeaderHub::LeaderHub() = default;

LeaderHub::~LeaderHub()
{
    stop(0);
}

bool LeaderHub::start(KeyRelay::Sender keySender, TextSender textSender,
                      std::unique_ptr<MemberLauncher> launcher)
{
    if (m_Thread != nullptr) {
        return false;
    }
    m_ServerName = QStringLiteral("moonlight-display-group-%1")
                       .arg(QUuid::createUuid().toString(QUuid::Id128));
    m_Thread = new QThread();
    m_Thread->setObjectName(QStringLiteral("DisplayGroupHub"));
    m_Worker =
        new LeaderHubWorker(std::move(keySender), std::move(textSender), std::move(launcher));
    m_Worker->moveToThread(m_Thread);
    QObject::connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start();

    bool listening = false;
    const QString name = m_ServerName;
    LeaderHubWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker, name, &listening] { listening = worker->listen(name); },
        Qt::BlockingQueuedConnection);
    if (!listening) {
        stop(0);
        return false;
    }
    return true;
}

QString LeaderHub::serverName() const
{
    return m_ServerName;
}

bool LeaderHub::isRunning() const
{
    return m_Thread != nullptr;
}

void LeaderHub::launchMembers(const QVector<QPair<int, QStringList>>& members)
{
    if (m_Worker == nullptr) {
        return;
    }
    LeaderHubWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker, members] { worker->launch(members); }, Qt::QueuedConnection);
}

void LeaderHub::stop(int graceMs)
{
    if (m_Thread == nullptr) {
        return;
    }
    LeaderHubWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker, graceMs] { worker->shutdown(graceMs); }, Qt::BlockingQueuedConnection);
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
    m_Thread = nullptr;
    m_Worker = nullptr;
}

int LeaderHub::connectedMembers() const
{
    return m_Worker != nullptr ? m_Worker->connectedCount.load() : 0;
}

int LeaderHub::launchedMembers() const
{
    return m_Worker != nullptr ? m_Worker->launchedCount.load() : 0;
}

class MemberLinkWorker : public QObject
{
public:
    explicit MemberLinkWorker(MemberLink::EndedCallback ended) : m_Ended(std::move(ended)) {}

    bool connectToLeader(const QString& name, int head, int timeoutMs)
    {
        m_Socket = new QLocalSocket(this);
        m_Socket->connectToServer(name);
        if (!m_Socket->waitForConnected(timeoutMs)) {
            qWarning() << "Display group member could not reach its leader:"
                       << m_Socket->errorString();
            return false;
        }
        connect(m_Socket, &QLocalSocket::readyRead, this, [this] {
            while (m_Socket->canReadLine()) {
                if (decodeIpcLine(m_Socket->readLine().trimmed()).type == IpcMessage::Quit) {
                    qInfo() << "Display group leader ended the group";
                    reportEnded();
                }
            }
        });
        connect(m_Socket, &QLocalSocket::disconnected, this, [this] {
            connected = false;
            if (!m_Stopping) {
                qWarning() << "Display group leader link dropped";
                reportEnded();
            }
        });

        IpcMessage hello;
        hello.type = IpcMessage::Hello;
        hello.version = kIpcProtocolVersion;
        hello.head = head;
        send(encodeIpc(hello));
        connected = true;
        return true;
    }

    void send(const QByteArray& line)
    {
        if (m_Socket != nullptr && m_Socket->state() == QLocalSocket::ConnectedState) {
            m_Socket->write(line);
            m_Socket->flush();
        }
    }

    void shutdown()
    {
        m_Stopping = true;
        connected = false;
        if (m_Socket != nullptr) {
            m_Socket->waitForBytesWritten(200);
            m_Socket->disconnectFromServer();
        }
    }

    std::atomic<bool> connected{ false };

private:
    void reportEnded()
    {
        if (!m_Reported && !m_Stopping && m_Ended) {
            m_Reported = true;
            m_Ended();
        }
    }

    MemberLink::EndedCallback m_Ended;
    QLocalSocket* m_Socket = nullptr;
    bool m_Stopping = false;
    bool m_Reported = false;
};

MemberLink::MemberLink() = default;

MemberLink::~MemberLink()
{
    stop();
}

bool MemberLink::start(const QString& serverName, int head, EndedCallback ended,
                       int connectTimeoutMs)
{
    if (m_Thread != nullptr) {
        return false;
    }
    m_Thread = new QThread();
    m_Thread->setObjectName(QStringLiteral("DisplayGroupLink"));
    m_Worker = new MemberLinkWorker(std::move(ended));
    m_Worker->moveToThread(m_Thread);
    QObject::connect(m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread->start();

    bool connected = false;
    MemberLinkWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker,
        [worker, serverName, head, connectTimeoutMs, &connected] {
            connected = worker->connectToLeader(serverName, head, connectTimeoutMs);
        },
        Qt::BlockingQueuedConnection);
    if (!connected) {
        stop();
        return false;
    }
    return true;
}

bool MemberLink::isConnected() const
{
    return m_Worker != nullptr && m_Worker->connected.load();
}

int MemberLink::sendKey(short code, char action, char modifiers, char flags)
{
    if (!isConnected()) {
        return -1;
    }
    IpcMessage message;
    message.type = IpcMessage::Key;
    message.code = code;
    message.action = action;
    message.modifiers = modifiers;
    message.flags = flags;
    const QByteArray line = encodeIpc(message);
    MemberLinkWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker, line] { worker->send(line); }, Qt::QueuedConnection);
    return 0;
}

int MemberLink::sendText(const QByteArray& utf8)
{
    if (!isConnected()) {
        return -1;
    }
    IpcMessage message;
    message.type = IpcMessage::Text;
    message.text = utf8;
    const QByteArray line = encodeIpc(message);
    MemberLinkWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker, line] { worker->send(line); }, Qt::QueuedConnection);
    return 0;
}

void MemberLink::stop()
{
    if (m_Thread == nullptr) {
        return;
    }
    MemberLinkWorker* worker = m_Worker;
    QMetaObject::invokeMethod(
        m_Worker, [worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
    m_Thread = nullptr;
    m_Worker = nullptr;
}

} // namespace DisplayGroup
