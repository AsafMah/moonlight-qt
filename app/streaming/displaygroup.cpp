#include "streaming/displaygroup.h"

#include <Limelight.h>

#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace DisplayGroup {

namespace {

QString xmlElementText(const QString& xml, const QString& element)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement && reader.name() == element) {
            return reader.readElementText();
        }
    }
    return {};
}

} // namespace

bool isValidGroupId(const QString& groupId)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(groupId).hasMatch();
}

QString groupIdToHex(const unsigned char (&groupId)[16])
{
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(groupId), sizeof(groupId)).toHex());
}

int hostMaxHeads(const QString& serverInfoXml)
{
    bool ok = false;
    const int heads =
        xmlElementText(serverInfoXml, QStringLiteral("DisplayGroupMaxHeads")).trimmed().toInt(&ok);
    if (!ok || heads < 2) {
        return 0;
    }
    return std::min(heads, kMaxHeads);
}

int leaderHeadCount(int requestedHeads, int hostMaxHeads)
{
    if (requestedHeads < 2 || hostMaxHeads < 2) {
        return 0;
    }
    return std::min({ requestedHeads, hostMaxHeads, kMaxHeads });
}

QString launchQueryArgs(const SessionOptions& options, bool resuming)
{
    switch (options.role) {
    case Role::Leader:
        // A group only exists for a fresh /launch; the host turns any later
        // /resume of the leader into an ordinary single-head session.
        if (resuming || options.requestedHeads < 2) {
            return {};
        }
        return QStringLiteral("&displayHeads=%1").arg(options.requestedHeads);
    case Role::Member:
        return QStringLiteral("&displayGroup=%1&displayHead=%2")
            .arg(options.groupId)
            .arg(options.head);
    case Role::None:
        break;
    }
    return {};
}

QString groupIdFromLaunchResponse(const QString& launchResponseXml)
{
    const QString groupId =
        xmlElementText(launchResponseXml, QStringLiteral("displayGroup")).trimmed();
    return isValidGroupId(groupId) ? groupId : QString();
}

bool validateMemberOptions(const SessionOptions& options, QString* error)
{
    QString problem;
    if (options.role != Role::Member) {
        problem = QStringLiteral("not a member");
    } else if (!isValidGroupId(options.groupId)) {
        problem = QStringLiteral("display group id must be 32 lowercase hex characters");
    } else if (options.head < 1 || options.head >= kMaxHeads) {
        problem = QStringLiteral("display head must be between 1 and %1").arg(kMaxHeads - 1);
    } else if (options.ipcName.isEmpty()) {
        problem = QStringLiteral("a display group member needs its leader's socket");
    } else if (options.width < 0 || options.height < 0 || options.fps < 0) {
        problem = QStringLiteral("invalid member stream size");
    }
    if (error != nullptr) {
        *error = problem;
    }
    return problem.isEmpty();
}

Layout layoutFromInfo(const _DISPLAY_GROUP_INFO& info)
{
    Layout layout;
    layout.groupId = groupIdToHex(info.groupId);
    layout.thisHead = info.thisHead;
    layout.fromHost = true;
    for (int i = 0; i < info.headCount && i < LI_DISPLAY_GROUP_MAX_HEADS; i++) {
        const DISPLAY_GROUP_HEAD& source = info.heads[i];
        Head head;
        head.index = source.index;
        head.admission = source.admission;
        head.reason = source.reason;
        head.x = source.desktopX;
        head.y = source.desktopY;
        head.width = int(std::min<uint32_t>(source.width, 16384));
        head.height = int(std::min<uint32_t>(source.height, 16384));
        head.refreshMilliHz = int(std::min<uint32_t>(source.refreshMilliHz, 1000000));
        layout.heads.append(head);
    }
    return layout;
}

Layout fallbackLayout(const QString& groupId, int headCount)
{
    Layout layout;
    layout.groupId = groupId;
    for (int i = 0; i < std::min(headCount, kMaxHeads); i++) {
        Head head;
        head.index = i;
        head.admission = i == 0 ? HeadStreaming : HeadPending;
        head.x = i;
        layout.heads.append(head);
    }
    return layout;
}

QVector<MemberPlan> planMembers(const Layout& layout, const QVector<ClientDisplay>& displays,
                                int leaderDisplayIndex, int fallbackWidth, int fallbackHeight)
{
    QVector<Head> heads;
    for (const Head& head : layout.heads) {
        if (head.index != 0 && head.admission != HeadRefused && head.admission != HeadEnded) {
            heads.append(head);
        }
    }
    std::stable_sort(heads.begin(), heads.end(), [](const Head& a, const Head& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });

    QVector<ClientDisplay> freeDisplays;
    for (const ClientDisplay& display : displays) {
        if (display.index != leaderDisplayIndex) {
            freeDisplays.append(display);
        }
    }
    std::stable_sort(freeDisplays.begin(), freeDisplays.end(),
                     [](const ClientDisplay& a, const ClientDisplay& b) {
                         return a.bounds.x() != b.bounds.x() ? a.bounds.x() < b.bounds.x()
                                                             : a.bounds.y() < b.bounds.y();
                     });

    QVector<MemberPlan> plans;
    for (int i = 0; i < heads.size(); i++) {
        MemberPlan plan;
        plan.head = heads[i].index;
        const bool hostSize = heads[i].width > 0 && heads[i].height > 0;
        plan.width = hostSize ? heads[i].width : fallbackWidth;
        plan.height = hostSize ? heads[i].height : fallbackHeight;
        if (i < freeDisplays.size()) {
            plan.displayIndex = freeDisplays[i].index;
            plan.fullscreen = true;
        } else {
            plan.displayIndex = leaderDisplayIndex;
            plan.fullscreen = false;
        }
        plans.append(plan);
    }
    return plans;
}

QStringList memberArguments(const QString& hostId, const QString& appName, const QString& groupId,
                            const QString& ipcName, int fps, const MemberPlan& plan)
{
    QStringList args = { QStringLiteral("stream"),
                         hostId,
                         appName,
                         QStringLiteral("--display-group"),
                         groupId,
                         QStringLiteral("--display-head"),
                         QString::number(plan.head),
                         QStringLiteral("--display-group-ipc"),
                         ipcName,
                         QStringLiteral("--display-group-size"),
                         QStringLiteral("%1x%2").arg(plan.width).arg(plan.height),
                         QStringLiteral("--display-group-fps"),
                         QString::number(fps),
                         QStringLiteral("--display-group-display"),
                         QString::number(plan.displayIndex) };
    if (plan.fullscreen) {
        args.append(QStringLiteral("--display-group-fullscreen"));
    }
    return args;
}

QByteArray encodeIpc(const IpcMessage& message)
{
    switch (message.type) {
    case IpcMessage::Hello:
        return QByteArray("H ") + QByteArray::number(message.version) + ' ' +
               QByteArray::number(message.head) + '\n';
    case IpcMessage::Key:
        return QByteArray("K ") + QByteArray::number(quint16(message.code)) + ' ' +
               QByteArray::number(quint8(message.action)) + ' ' +
               QByteArray::number(quint8(message.modifiers)) + ' ' +
               QByteArray::number(quint8(message.flags)) + '\n';
    case IpcMessage::Text:
        return QByteArray("T ") + message.text.toBase64() + '\n';
    case IpcMessage::Quit:
        return QByteArrayLiteral("Q\n");
    case IpcMessage::Invalid:
        break;
    }
    return {};
}

IpcMessage decodeIpcLine(const QByteArray& line)
{
    IpcMessage message;
    const QList<QByteArray> parts = line.split(' ');
    if (parts.isEmpty() || parts[0].size() != 1) {
        return message;
    }

    auto number = [](const QByteArray& token, uint max, uint* out) {
        bool ok = false;
        const uint value = token.toUInt(&ok);
        if (!ok || value > max) {
            return false;
        }
        *out = value;
        return true;
    };

    switch (parts[0][0]) {
    case 'H': {
        uint version = 0;
        uint head = 0;
        if (parts.size() == 3 && number(parts[1], 255, &version) &&
            number(parts[2], kMaxHeads - 1, &head)) {
            message.type = IpcMessage::Hello;
            message.version = int(version);
            message.head = int(head);
        }
        break;
    }
    case 'K': {
        uint code = 0;
        uint action = 0;
        uint modifiers = 0;
        uint flags = 0;
        if (parts.size() == 5 && number(parts[1], 0xFFFF, &code) &&
            number(parts[2], 0xFF, &action) && number(parts[3], 0xFF, &modifiers) &&
            number(parts[4], 0xFF, &flags) &&
            (action == KEY_ACTION_DOWN || action == KEY_ACTION_UP)) {
            message.type = IpcMessage::Key;
            message.code = short(quint16(code));
            message.action = char(action);
            message.modifiers = char(modifiers);
            message.flags = char(flags);
        }
        break;
    }
    case 'T': {
        if (parts.size() == 2) {
            const auto decoded =
                QByteArray::fromBase64Encoding(parts[1], QByteArray::AbortOnBase64DecodingErrors);
            if (decoded) {
                message.type = IpcMessage::Text;
                message.text = *decoded;
            }
        }
        break;
    }
    case 'Q':
        if (parts.size() == 1) {
            message.type = IpcMessage::Quit;
        }
        break;
    default:
        break;
    }
    return message;
}

KeyRelay::KeyRelay(Sender sender) : m_Sender(std::move(sender)) {}

void KeyRelay::forward(int member, short code, char action, char modifiers, char flags)
{
    if (action == KEY_ACTION_DOWN) {
        m_Down[member].insert(code, DownKey{ modifiers, flags });
    } else {
        auto it = m_Down.find(member);
        if (it != m_Down.end()) {
            it->remove(code);
        }
    }
    m_Sender(code, action, modifiers, flags);
}

void KeyRelay::releaseMember(int member)
{
    const QHash<short, DownKey> keys = m_Down.take(member);
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        m_Sender(it.key(), KEY_ACTION_UP, it.value().modifiers, it.value().flags);
    }
}

int KeyRelay::keysDown(int member) const
{
    return int(m_Down.value(member).size());
}

} // namespace DisplayGroup
