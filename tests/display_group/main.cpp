// Display group (Option G) client logic: negotiation, layout, member planning,
// IPC framing and leader-side key relay. The fake host here is a byte builder
// for the 0x55F0 layout message, parsed with the production common-c parser.

#include "streaming/displaygroup.h"

#include "DisplayGroupStream.h"

#include <QCoreApplication>
#include <QTextStream>

int runHubTests(QTextStream& err);  // hub_tests.cpp
int runSinkTests(QTextStream& err); // sink_tests.cpp

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

void put32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

struct FakeHostHead
{
    uint8_t admission;
    uint16_t reason;
    int32_t x, y;
    uint32_t w, h, refresh;
};

QByteArray fakeHostLayout(uint8_t thisHead, const QVector<FakeHostHead>& heads)
{
    QByteArray wire(20 + 24 * heads.size(), '\0');
    auto* p = reinterpret_cast<uint8_t*>(wire.data());
    p[0] = 1;
    p[1] = uint8_t(heads.size());
    p[2] = thisHead;
    for (int i = 0; i < 16; i++) {
        p[4 + i] = uint8_t(0x11 * i);
    }
    for (int i = 0; i < heads.size(); i++) {
        uint8_t* e = p + 20 + 24 * i;
        e[0] = uint8_t(i);
        e[1] = heads[i].admission;
        e[2] = uint8_t(heads[i].reason);
        e[3] = uint8_t(heads[i].reason >> 8);
        put32(e + 4, uint32_t(heads[i].x));
        put32(e + 8, uint32_t(heads[i].y));
        put32(e + 12, heads[i].w);
        put32(e + 16, heads[i].h);
        put32(e + 20, heads[i].refresh);
    }
    return wire;
}

bool parseFakeHost(const QByteArray& wire, DISPLAY_GROUP_INFO* info)
{
    return parseDisplayGroupMessage(reinterpret_cast<const uint8_t*>(wire.constData()),
                                    int(wire.size()), info);
}

const QString kGroup = QStringLiteral("00112233445566778899aabbccddeeff");

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    bool ok = true;
    using namespace DisplayGroup;

    // --- Group ids ---
    ok &= require(isValidGroupId(kGroup), "32 lowercase hex chars is a group id", err);
    ok &= require(!isValidGroupId(kGroup.toUpper()), "uppercase is rejected", err);
    ok &= require(!isValidGroupId(kGroup.left(31)), "short id is rejected", err);
    ok &= require(!isValidGroupId(kGroup + "0"), "long id is rejected", err);
    ok &= require(!isValidGroupId(QString(kGroup).replace(0, 1, "g")), "non-hex rejected", err);
    ok &= require(!isValidGroupId(QString()), "empty id is rejected", err);

    // --- Capability and negotiation ---
    ok &= require(hostMaxHeads("<root status_code=\"200\"><state>X</state></root>") == 0,
                  "a legacy host has no capability", err);
    ok &= require(hostMaxHeads("<root status_code=\"200\"><DisplayGroupMaxHeads>2"
                               "</DisplayGroupMaxHeads></root>") == 2,
                  "capability is read from serverinfo", err);
    ok &= require(hostMaxHeads("<root><DisplayGroupMaxHeads>x</DisplayGroupMaxHeads></root>") == 0,
                  "garbage capability is 0", err);
    ok &= require(hostMaxHeads("<root><DisplayGroupMaxHeads>99</DisplayGroupMaxHeads></root>") ==
                      kMaxHeads,
                  "capability is capped at the protocol maximum", err);
    ok &= require(hostMaxHeads("not xml") == 0, "malformed serverinfo is 0", err);

    ok &= require(leaderHeadCount(0, 2) == 0, "off when the user did not opt in", err);
    ok &= require(leaderHeadCount(1, 2) == 0, "one head is not a group", err);
    ok &= require(leaderHeadCount(2, 0) == 0, "off against a legacy host", err);
    ok &= require(leaderHeadCount(2, 1) == 0, "off when the host serves one head", err);
    ok &= require(leaderHeadCount(2, 2) == 2, "two heads when both agree", err);
    ok &= require(leaderHeadCount(4, 2) == 2, "capped by the host", err);
    ok &= require(leaderHeadCount(3, 8) == 3, "user choice under the cap", err);

    SessionOptions none;
    ok &= require(launchQueryArgs(none, false).isEmpty() && launchQueryArgs(none, true).isEmpty(),
                  "an ordinary session adds nothing to /launch or /resume", err);

    SessionOptions leader;
    leader.role = Role::Leader;
    leader.requestedHeads = 2;
    ok &= require(launchQueryArgs(leader, false) == "&displayHeads=2",
                  "leader /launch requests the heads", err);
    ok &= require(launchQueryArgs(leader, true).isEmpty(),
                  "a leader reconnect is a plain resume (the host ended the group)", err);
    leader.requestedHeads = 1;
    ok &= require(launchQueryArgs(leader, false).isEmpty(), "leader with 1 head adds nothing", err);

    SessionOptions member;
    member.role = Role::Member;
    member.groupId = kGroup;
    member.head = 1;
    member.ipcName = "moonlight-dg-test";
    ok &= require(launchQueryArgs(member, true) == "&displayGroup=" + kGroup + "&displayHead=1",
                  "member /resume joins its head", err);
    ok &= require(launchQueryArgs(member, false) == "&displayGroup=" + kGroup + "&displayHead=1",
                  "member args do not depend on the verb", err);

    QString error;
    ok &= require(validateMemberOptions(member, &error), "valid member options: " + error, err);
    SessionOptions bad = member;
    bad.head = 0;
    ok &= require(!validateMemberOptions(bad, &error), "head 0 is the leader's", err);
    bad = member;
    bad.head = kMaxHeads;
    ok &= require(!validateMemberOptions(bad, &error), "head out of range", err);
    bad = member;
    bad.groupId = "nope";
    ok &= require(!validateMemberOptions(bad, &error), "bad group id", err);
    bad = member;
    bad.ipcName.clear();
    ok &= require(!validateMemberOptions(bad, &error), "a member needs the leader socket", err);

    ok &= require(groupIdFromLaunchResponse(
                      "<root status_code=\"200\"><sessionUrl0>rtsp://h:48010</sessionUrl0>"
                      "<gamesession>1</gamesession><displayGroup>" +
                      kGroup + "</displayGroup></root>") == kGroup,
                  "group id is read from the launch response", err);
    ok &= require(groupIdFromLaunchResponse("<root status_code=\"200\"><sessionUrl0>x"
                                            "</sessionUrl0></root>")
                      .isEmpty(),
                  "no group id means N=1", err);
    ok &= require(
        groupIdFromLaunchResponse("<root><displayGroup>xyz</displayGroup></root>").isEmpty(),
        "an invalid group id means N=1", err);

    // --- Layout from the fake host ---
    DISPLAY_GROUP_INFO info;
    const QByteArray wire =
        fakeHostLayout(0, { { HeadStreaming, 0, 0, 0, 2560, 1440, 144000 },
                            { HeadPending, 0, -1920, 180, 1920, 1080, 60000 } });
    ok &= require(parseFakeHost(wire, &info), "fake host layout parses", err);
    Layout layout = layoutFromInfo(info);
    ok &= require(layout.fromHost && layout.thisHead == 0 && layout.heads.size() == 2,
                  "layout shape", err);
    ok &= require(layout.groupId == "00112233445566778899aabbccddeeff",
                  "group id bytes map to lowercase hex in order: " + layout.groupId, err);
    if (layout.heads.size() == 2) {
        ok &= require(layout.heads[1].x == -1920 && layout.heads[1].y == 180 &&
                          layout.heads[1].width == 1920 && layout.heads[1].height == 1080 &&
                          layout.heads[1].refreshMilliHz == 60000 &&
                          layout.heads[1].admission == HeadPending,
                      "head fields survive", err);
    }

    // --- Member planning ---
    const QVector<ClientDisplay> twoMonitors = { { 0, QRect(0, 0, 2560, 1440) },
                                                 { 1, QRect(2560, 0, 1920, 1080) } };
    QVector<MemberPlan> plans = planMembers(layout, twoMonitors, 0, 2560, 1440);
    ok &= require(plans.size() == 1 && plans[0].head == 1 && plans[0].displayIndex == 1 &&
                      plans[0].fullscreen && plans[0].width == 1920 && plans[0].height == 1080,
                  "member gets the free monitor, fullscreen, at its head size", err);

    plans = planMembers(layout, twoMonitors, 1, 2560, 1440);
    ok &= require(plans.size() == 1 && plans[0].displayIndex == 0 && plans[0].fullscreen,
                  "the member takes whichever monitor the leader does not use", err);

    plans = planMembers(layout, { { 0, QRect(0, 0, 2560, 1440) } }, 0, 2560, 1440);
    ok &= require(plans.size() == 1 && plans[0].displayIndex == 0 && !plans[0].fullscreen,
                  "without a free monitor the member is windowed on the leader's display", err);

    // Three heads on three monitors: host desktop order maps to client order.
    Layout three = fallbackLayout(kGroup, 3);
    if (three.heads.size() == 3) {
        three.heads[1].x = 3840;
        three.heads[2].x = -1920;
    }
    const QVector<ClientDisplay> threeMonitors = { { 0, QRect(0, 0, 1920, 1080) },
                                                   { 1, QRect(1920, 0, 1920, 1080) },
                                                   { 2, QRect(-1920, 0, 1920, 1080) } };
    plans = planMembers(three, threeMonitors, 0, 1920, 1080);
    ok &= require(plans.size() == 2, "two members for three heads", err);
    if (plans.size() == 2) {
        ok &= require(plans[0].head == 2 && plans[0].displayIndex == 2,
                      "the leftmost host head goes to the leftmost free monitor", err);
        ok &= require(plans[1].head == 1 && plans[1].displayIndex == 1,
                      "the rightmost host head goes to the rightmost free monitor", err);
        ok &= require(plans[0].width == 1920 && plans[0].height == 1080,
                      "fallback layout uses the leader's stream size", err);
    }

    Layout refused = layout;
    if (refused.heads.size() == 2) {
        refused.heads[1].admission = HeadRefused;
        ok &= require(planMembers(refused, twoMonitors, 0, 2560, 1440).isEmpty(),
                      "refused heads are not streamed", err);
        refused.heads[1].admission = HeadEnded;
        ok &= require(planMembers(refused, twoMonitors, 0, 2560, 1440).isEmpty(),
                      "ended heads are not streamed", err);
    }

    Layout fb = fallbackLayout(kGroup, 2);
    ok &= require(!fb.fromHost && fb.heads.size() == 2 && fb.groupId == kGroup,
                  "fallback layout shape", err);

    // --- Member command line ---
    MemberPlan plan;
    plan.head = 1;
    plan.width = 1920;
    plan.height = 1080;
    plan.displayIndex = 1;
    plan.fullscreen = true;
    const QStringList args =
        memberArguments("HOST-UUID", "Desktop", kGroup, "moonlight-dg-x", 120, plan);
    const QStringList expected = { "stream",
                                   "HOST-UUID",
                                   "Desktop",
                                   "--display-group",
                                   kGroup,
                                   "--display-head",
                                   "1",
                                   "--display-group-ipc",
                                   "moonlight-dg-x",
                                   "--display-group-size",
                                   "1920x1080",
                                   "--display-group-fps",
                                   "120",
                                   "--display-group-display",
                                   "1",
                                   "--display-group-fullscreen" };
    ok &= require(args == expected, "member argv: " + args.join(' '), err);
    plan.fullscreen = false;
    ok &= require(
        !memberArguments("h", "a", kGroup, "i", 60, plan).contains("--display-group-fullscreen"),
        "windowed members omit the fullscreen flag", err);

    // --- IPC codec ---
    IpcMessage hello;
    hello.type = IpcMessage::Hello;
    hello.version = kIpcProtocolVersion;
    hello.head = 1;
    ok &= require(encodeIpc(hello) == "H 1 1\n", "hello encoding", err);
    IpcMessage key;
    key.type = IpcMessage::Key;
    key.code = short(0x8041);
    key.action = 0x03;
    key.modifiers = 0x02;
    key.flags = 0x01;
    const QByteArray keyLine = encodeIpc(key);
    IpcMessage decoded = decodeIpcLine(keyLine.trimmed());
    ok &= require(decoded.type == IpcMessage::Key && decoded.code == short(0x8041) &&
                      decoded.action == 0x03 && decoded.modifiers == 0x02 && decoded.flags == 0x01,
                  "key round trip: " + QString::fromLatin1(keyLine), err);
    IpcMessage text;
    text.type = IpcMessage::Text;
    text.text = QByteArray("h\xC3\xA9llo\nworld");
    decoded = decodeIpcLine(encodeIpc(text).trimmed());
    ok &= require(decoded.type == IpcMessage::Text && decoded.text == text.text,
                  "utf8 text with newline round trips", err);
    IpcMessage quit;
    quit.type = IpcMessage::Quit;
    ok &= require(decodeIpcLine(encodeIpc(quit).trimmed()).type == IpcMessage::Quit,
                  "quit round trip", err);
    decoded = decodeIpcLine("H 1 1");
    ok &= require(decoded.type == IpcMessage::Hello && decoded.version == 1 && decoded.head == 1,
                  "hello decode", err);
    for (const char* garbage :
         { "", "Z", "K 1 2", "K a b c d", "H", "H 1 x", "K 70000 3 0 0", "K 1 9 0 0", "T !!!" }) {
        ok &= require(decodeIpcLine(garbage).type == IpcMessage::Invalid,
                      QString("garbage is invalid: '%1'").arg(garbage), err);
    }

    // --- Key relay ---
    struct Sent
    {
        short code;
        char action, modifiers, flags;
    };
    QVector<Sent> sent;
    KeyRelay relay([&sent](short code, char action, char modifiers, char flags) {
        sent.append({ code, action, modifiers, flags });
        return 0;
    });
    relay.forward(1, short(0x8041), 0x03, 0x02, 0x00);
    relay.forward(1, short(0x8010), 0x03, 0x00, 0x01);
    relay.forward(2, short(0x8042), 0x03, 0x00, 0x00);
    ok &= require(sent.size() == 3 && relay.keysDown(1) == 2 && relay.keysDown(2) == 1,
                  "keys are forwarded and tracked per member", err);
    relay.forward(1, short(0x8041), 0x04, 0x02, 0x00);
    ok &= require(relay.keysDown(1) == 1, "key up clears the key", err);
    relay.releaseMember(1);
    ok &= require(sent.size() == 5 && sent.last().code == short(0x8010) &&
                      sent.last().action == 0x04 && sent.last().flags == 0x01,
                  "a vanished member's held key is released with its flags", err);
    ok &= require(relay.keysDown(1) == 0 && relay.keysDown(2) == 1,
                  "release only affects that member", err);
    relay.releaseMember(1);
    ok &= require(sent.size() == 5, "releasing twice sends nothing", err);
    relay.forward(2, short(0x8042), 0x03, 0x00, 0x00);
    ok &= require(relay.keysDown(2) == 1, "auto-repeat downs do not double count", err);

    // --- Leader hub with fake member processes over real local sockets ---
    ok &= runHubTests(err) == 0;
    ok &= runSinkTests(err) == 0;

    if (!ok) {
        err << "display_group: FAILED\n";
        return 1;
    }
    QTextStream(stdout) << "display_group: all tests passed\n";
    return 0;
}
