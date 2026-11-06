#pragma once

// Display groups (Option G): several host display heads streamed as a group of
// ordinary single-track sessions, one Moonlight process and window per head.
// The leader process launches the app with N heads and starts one member
// process per extra head; members join with /resume. All keyboard input goes
// through the leader's session. Everything here is inert unless the user opted
// in and the host advertised DisplayGroupMaxHeads >= 2.
//
// This file holds the pure, Qt-Core-only parts so they can be tested directly.

#include <QByteArray>
#include <QHash>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct _DISPLAY_GROUP_INFO;

namespace DisplayGroup {

constexpr int kMaxHeads = 8;
constexpr int kIpcProtocolVersion = 1;

enum class Role
{
    None,
    Leader,
    Member
};

// Admission states and reasons mirror LI_DISPLAY_HEAD_* in Limelight.h.
enum HeadAdmission
{
    HeadPending = 0,
    HeadStreaming = 1,
    HeadRefused = 2,
    HeadEnded = 3
};

struct Head
{
    int index = 0;
    int admission = HeadPending;
    int reason = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
};

struct Layout
{
    QString groupId;
    int thisHead = 0;
    QVector<Head> heads;
    // True when built from the host's layout message, false for the fallback
    // used when a host issued a group id but never sent a layout.
    bool fromHost = false;
};

// Per-session group state. Role::None means an ordinary session and must
// leave every request byte-for-byte unchanged.
struct SessionOptions
{
    Role role = Role::None;
    int requestedHeads = 0; // Leader: heads asked for (>= 2), before the host cap
    QString groupId;        // Member: group to join. Leader: issued by /launch
    int head = 0;           // Member: head index (1..N-1)
    QString ipcName;        // Member: leader's local socket name
    int width = 0;          // Member: stream size (0 = preferences)
    int height = 0;
    int fps = 0;           // Member: stream fps (0 = preferences)
    int displayIndex = -1; // Member: client display for the window (-1 = default)
    bool fullscreen = false;
};

bool isValidGroupId(const QString& groupId);
QString groupIdToHex(const unsigned char (&groupId)[16]);

// DisplayGroupMaxHeads from a /serverinfo response, or 0 when absent/invalid.
int hostMaxHeads(const QString& serverInfoXml);

// The head count a leader should request: 0 (no group) unless the user asked
// for >= 2 heads and the host can serve >= 2.
int leaderHeadCount(int requestedHeads, int hostMaxHeads);

// Query suffix appended to /launch or /resume. Empty for Role::None.
QString launchQueryArgs(const SessionOptions& options, bool resuming);

// The group id from a /launch response, or empty (treat as N=1).
QString groupIdFromLaunchResponse(const QString& launchResponseXml);

bool validateMemberOptions(const SessionOptions& options, QString* error);

Layout layoutFromInfo(const _DISPLAY_GROUP_INFO& info);
Layout fallbackLayout(const QString& groupId, int headCount);

struct ClientDisplay
{
    int index = 0;
    QRect bounds;
};

struct MemberPlan
{
    int head = 0;
    int width = 0;
    int height = 0;
    int displayIndex = -1;
    bool fullscreen = false;
};

// Places member heads on the client displays the leader does not use, in
// host desktop order. Members without a free display open windowed on the
// leader's display. Refused and ended heads are skipped.
QVector<MemberPlan> planMembers(const Layout& layout, const QVector<ClientDisplay>& displays,
                                int leaderDisplayIndex, int fallbackWidth, int fallbackHeight);

QStringList memberArguments(const QString& hostId, const QString& appName, const QString& groupId,
                            const QString& ipcName, int fps, const MemberPlan& plan);

// Leader <-> member local socket protocol: one ASCII line per message.
//   member -> leader: "H <version> <head>"            hello
//                     "K <code> <action> <mods> <flags>" keyboard event
//                     "T <base64 utf8>"                 text event
//   leader -> member: "Q"                              group ended, exit
struct IpcMessage
{
    enum Type
    {
        Invalid,
        Hello,
        Key,
        Text,
        Quit
    };
    Type type = Invalid;
    int version = 0;
    int head = 0;
    short code = 0;
    char action = 0;
    char modifiers = 0;
    char flags = 0;
    QByteArray text;
};

QByteArray encodeIpc(const IpcMessage& message);
IpcMessage decodeIpcLine(const QByteArray& line);

// Leader-side keyboard bookkeeping. The host has one keyboard state, so every
// forwarded key passes through the leader session. Keys still down when a
// member goes away are released so they cannot stick.
class KeyRelay
{
public:
    using Sender = std::function<int(short code, char action, char modifiers, char flags)>;

    explicit KeyRelay(Sender sender);

    void forward(int member, short code, char action, char modifiers, char flags);
    void releaseMember(int member);
    int keysDown(int member) const;

private:
    struct DownKey
    {
        char modifiers;
        char flags;
    };

    Sender m_Sender;
    QHash<int, QHash<short, DownKey>> m_Down;
};

} // namespace DisplayGroup
