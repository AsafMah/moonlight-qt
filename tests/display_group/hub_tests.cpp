// LeaderHub / MemberLink over real local sockets. FakeLauncher stands in for
// member processes: launching a head starts an in-process MemberLink.

#include "streaming/displaygroupipc.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QTextStream>
#include <QThread>

#include <atomic>

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: hub: " << message << '\n';
    }
    return condition;
}

template <typename Predicate> bool waitFor(Predicate predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

struct Recorder
{
    struct Key
    {
        short code;
        char action, modifiers, flags;
    };
    mutable QMutex lock;
    QVector<Key> keys;
    QVector<QByteArray> texts;
    int keyCount() const
    {
        QMutexLocker l(&lock);
        return int(keys.size());
    }
    Key lastKey() const
    {
        QMutexLocker l(&lock);
        return keys.last();
    }
};

struct FakeMembers
{
    QMutex lock;
    QVector<QPair<int, QStringList>> launches;
    std::atomic<int> terminateCalls{ 0 };
    std::atomic<int> ended{ 0 };
    QVector<std::shared_ptr<DisplayGroup::MemberLink>> links;
    QString serverName;
};

class FakeLauncher : public DisplayGroup::MemberLauncher
{
public:
    explicit FakeLauncher(FakeMembers* members) : m_Members(members) {}

    void launch(int head, const QStringList& arguments) override
    {
        auto link = std::make_shared<DisplayGroup::MemberLink>();
        FakeMembers* members = m_Members;
        link->start(members->serverName, head, [members] { members->ended++; });
        QMutexLocker l(&members->lock);
        members->launches.append({ head, arguments });
        members->links.append(link);
    }

    void terminateAll(int) override { m_Members->terminateCalls++; }

private:
    FakeMembers* m_Members;
};

} // namespace

int runHubTests(QTextStream& err)
{
    using namespace DisplayGroup;
    bool ok = true;

    Recorder recorder;
    FakeMembers members;
    LeaderHub hub;
    ok &= require(hub.start(
                      [&recorder](short code, char action, char modifiers, char flags) {
                          QMutexLocker l(&recorder.lock);
                          recorder.keys.append({ code, action, modifiers, flags });
                          return 0;
                      },
                      [&recorder](const QByteArray& utf8) {
                          QMutexLocker l(&recorder.lock);
                          recorder.texts.append(utf8);
                          return 0;
                      },
                      std::make_unique<FakeLauncher>(&members)),
                  "hub starts", err);
    members.serverName = hub.serverName();
    ok &= require(!hub.serverName().isEmpty(), "hub has a socket name", err);

    hub.launchMembers({ { 1, { "stream", "host", "app", "--display-head", "1" } } });
    ok &= require(waitFor([&] { return hub.connectedMembers() == 1; }),
                  "the launched member connects", err);
    {
        QMutexLocker l(&members.lock);
        ok &= require(members.launches.size() == 1 && members.launches[0].first == 1 &&
                          members.launches[0].second.contains("--display-head"),
                      "the launcher got the member arguments", err);
    }

    // A repeated layout message must not start a second process for head 1.
    hub.launchMembers(
        { { 1, { "again" } }, { 2, { "stream", "host", "app", "--display-head", "2" } } });
    ok &= require(waitFor([&] { return hub.connectedMembers() == 2; }), "head 2 connects", err);
    ok &= require(hub.launchedMembers() == 2, "head 1 was not launched twice", err);

    std::shared_ptr<MemberLink> head1;
    std::shared_ptr<MemberLink> head2;
    {
        QMutexLocker l(&members.lock);
        head1 = members.links.value(0);
        head2 = members.links.value(1);
    }
    ok &= require(head1 && head1->isConnected() && head2 && head2->isConnected(),
                  "both links are connected", err);
    if (!head1 || !head2) {
        hub.stop();
        return 1;
    }

    // Keyboard from member windows goes out on the leader's session, in order.
    head1->sendKey(short(0x8041), 0x03, 0x02, 0x00);
    head1->sendKey(short(0x8010), 0x03, 0x00, 0x01);
    head1->sendKey(short(0x8041), 0x04, 0x02, 0x00);
    ok &= require(waitFor([&] { return recorder.keyCount() == 3; }), "keys reach the leader", err);
    {
        QMutexLocker l(&recorder.lock);
        ok &= require(recorder.keys.size() == 3 && recorder.keys[0].code == short(0x8041) &&
                          recorder.keys[1].code == short(0x8010) &&
                          recorder.keys[2].action == 0x04 && recorder.keys[0].modifiers == 0x02,
                      "keys arrive in order with their fields", err);
    }
    head2->sendText(QByteArray("h\xC3\xA9"));
    ok &= require(waitFor([&] {
                      QMutexLocker l(&recorder.lock);
                      return recorder.texts.size() == 1;
                  }),
                  "text reaches the leader", err);

    // A member that goes away with a key held must not leave it stuck.
    head1->stop();
    ok &= require(waitFor([&] { return hub.connectedMembers() == 1; }),
                  "the hub notices the member left", err);
    ok &= require(waitFor([&] { return recorder.keyCount() == 4; }),
                  "the held key is released by the leader", err);
    if (recorder.keyCount() == 4) {
        const auto last = recorder.lastKey();
        ok &= require(last.code == short(0x8010) && last.action == 0x04 && last.flags == 0x01,
                      "the release matches the held key", err);
    }
    ok &= require(members.ended == 0, "a member stopping itself is not a group end", err);

    // Ending the leader ends every member and stops the launcher.
    hub.stop(100);
    ok &= require(waitFor([&] { return members.ended == 1; }),
                  "the remaining member is told to quit", err);
    ok &= require(members.terminateCalls == 1, "the launcher reaps the members", err);
    ok &= require(!hub.isRunning(), "the hub is stopped", err);
    hub.stop(100);
    ok &= require(members.terminateCalls == 1, "stopping twice is harmless", err);

    // A member whose leader vanished cannot start.
    MemberLink orphan;
    std::atomic<int> orphanEnded{ 0 };
    ok &= require(!orphan.start(
                      members.serverName, 1, [&] { orphanEnded++; }, 300),
                  "a member without a leader fails to start", err);

    // A leader crash (socket drop without "Q") also ends the member. The fake
    // leader is a bare server that accepts the hello and then drops the link.
    QLocalServer fakeLeader;
    const QString fakeName =
        QStringLiteral("moonlight-dg-test-%1").arg(QCoreApplication::applicationPid());
    QLocalServer::removeServer(fakeName);
    ok &= require(fakeLeader.listen(fakeName), "fake leader listens", err);
    MemberLink survivor;
    std::atomic<int> survivorEnded{ 0 };
    ok &= require(survivor.start(fakeName, 1, [&] { survivorEnded++; }), "member connects", err);
    ok &= require(waitFor([&] { return fakeLeader.hasPendingConnections(); }),
                  "fake leader sees it", err);
    QLocalSocket* accepted = fakeLeader.nextPendingConnection();
    ok &= require(accepted != nullptr && waitFor([&] {
                      return accepted->canReadLine() || accepted->waitForReadyRead(10);
                  }),
                  "member says hello", err);
    if (accepted != nullptr) {
        ok &= require(accepted->readLine().trimmed() == "H 1 1", "hello line", err);
        accepted->abort();
    }
    ok &= require(waitFor([&] { return survivorEnded == 1; }), "a dropped leader ends the member",
                  err);
    survivor.stop();
    ok &= require(survivorEnded == 1, "the end is reported once", err);

    // A client that never says hello is not counted as a member.
    LeaderHub strictHub;
    FakeMembers strictMembers;
    ok &= require(strictHub.start([](short, char, char, char) { return 0; },
                                  [](const QByteArray&) { return 0; },
                                  std::make_unique<FakeLauncher>(&strictMembers)),
                  "third hub starts", err);
    {
        QLocalSocket intruder;
        intruder.connectToServer(strictHub.serverName());
        ok &= require(intruder.waitForConnected(1000), "intruder connects", err);
        intruder.write("K 32833 3 0 0\n");
        intruder.flush();
        intruder.waitForBytesWritten(500);
        QThread::msleep(100);
        ok &= require(strictHub.connectedMembers() == 0, "no hello, no member", err);
    }
    strictHub.stop(100);

    return ok ? 0 : 1;
}
