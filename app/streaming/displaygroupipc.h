#pragma once

// Process plumbing for display groups. The leader runs a LeaderHub: a local
// socket server on its own thread that receives keyboard events from member
// processes and sends them on the leader's session, and that owns the member
// processes. Each member runs a MemberLink to its leader. A member treats a
// lost link as the end of the group, so members never outlive a crashed leader.
//
// Neither class depends on the Qt or SDL main loop: both run their sockets on a
// private thread, because the streaming loop only pumps Qt events on idle.

#include "streaming/displaygroup.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class QThread;

namespace DisplayGroup {

class MemberLauncher
{
public:
    virtual ~MemberLauncher() = default;
    // Both are called on the hub thread.
    virtual void launch(int head, const QStringList& arguments) = 0;
    // Members were already told to quit; wait up to graceMs, then kill.
    virtual void terminateAll(int graceMs) = 0;
};

// Starts members as child processes of this executable.
std::unique_ptr<MemberLauncher> createProcessMemberLauncher();

class LeaderHubWorker;

class LeaderHub
{
public:
    using TextSender = std::function<int(const QByteArray& utf8)>;

    LeaderHub();
    ~LeaderHub();

    // Listens on a fresh, user-private local socket. Senders run on the hub thread.
    bool start(KeyRelay::Sender keySender, TextSender textSender,
               std::unique_ptr<MemberLauncher> launcher);
    QString serverName() const;
    bool isRunning() const;

    // Launches each head at most once for the lifetime of the hub.
    void launchMembers(const QVector<QPair<int, QStringList>>& members);

    // Tells members to quit, releases their keys, stops the launcher and joins
    // the thread. Safe to call more than once.
    void stop(int graceMs = 3000);

    int connectedMembers() const;
    int launchedMembers() const;

private:
    QThread* m_Thread = nullptr;
    LeaderHubWorker* m_Worker = nullptr;
    QString m_ServerName;
};

class MemberLinkWorker;

class MemberLink
{
public:
    // Called on the link thread when the leader says the group ended or the
    // link drops. Called at most once.
    using EndedCallback = std::function<void()>;

    MemberLink();
    ~MemberLink();

    bool start(const QString& serverName, int head, EndedCallback ended,
               int connectTimeoutMs = 5000);
    bool isConnected() const;

    // Thread-safe; events are written in call order.
    int sendKey(short code, char action, char modifiers, char flags);
    int sendText(const QByteArray& utf8);

    void stop();

private:
    QThread* m_Thread = nullptr;
    MemberLinkWorker* m_Worker = nullptr;
};

} // namespace DisplayGroup
