// Display groups (Option G): one ordinary session per host display head, one
// process and window each. See displaygroup.h for the model and the wire
// contract. Every path here is inert for Role::None.

#include "session.h"

#include "backend/nvhttp.h"
#include "streaming/displaygroupipc.h"
#include "streaming/input/keyboardsink.h"

#include "SDL_compat.h"
#include <Limelight.h>

namespace {

constexpr Uint32 k_DisplayGroupLayoutTimeoutMs = 3000;
constexpr int k_DisplayGroupMemberGraceMs = 5000;

void pushDisplayGroupEvent(int code)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = code;
    if (SDL_PushEvent(&event) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to queue display group event: %s",
                    SDL_GetError());
    }
}

} // namespace

void Session::setDisplayGroupOptions(const DisplayGroup::SessionOptions& options)
{
    m_DisplayGroup = options;
    if (options.role == DisplayGroup::Role::Member) {
        m_IsFullScreen = options.fullscreen;
    }
}

void Session::setDisplayGroupLeaderHeads(int heads)
{
    // An explicit count (including 1 = off) overrides the saved preference.
    if (m_DisplayGroup.role != DisplayGroup::Role::Member) {
        m_DisplayGroup.role = DisplayGroup::Role::Leader;
        m_DisplayGroup.requestedHeads = heads;
    }
}

QString Session::prepareDisplayGroupLaunch(NvHTTP& http, bool resumingSession)
{
    if (isDisplayGroupMember()) {
        return DisplayGroup::launchQueryArgs(m_DisplayGroup, true);
    }

    const int requested = m_DisplayGroup.role == DisplayGroup::Role::Leader
                              ? m_DisplayGroup.requestedHeads
                              : m_Preferences->displayGroupHeads;
    if (requested < 2) {
        // Opted out: no capability probe, no extra query arguments.
        return {};
    }
    if (resumingSession) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: resuming streams one display; a group needs a fresh launch");
        return {};
    }

    int hostMaxHeads = 0;
    try {
        hostMaxHeads = DisplayGroup::hostMaxHeads(http.getServerInfo(NvHTTP::NVLL_ERROR, false));
    } catch (const GfeHttpResponseException& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Display group: serverinfo failed: %s",
                    qPrintable(e.toQString()));
    } catch (const QtNetworkReplyException& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Display group: serverinfo failed: %s",
                    qPrintable(e.toQString()));
    }

    const int heads = DisplayGroup::leaderHeadCount(requested, hostMaxHeads);
    if (heads < 2) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: host does not support display groups; streaming one display");
        m_DisplayGroup = {};
        return {};
    }

    m_DisplayGroup.role = DisplayGroup::Role::Leader;
    m_DisplayGroup.requestedHeads = heads;
    m_DisplayGroup.groupId.clear();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Display group: launching with %d heads (host max %d)", heads, hostMaxHeads);
    return DisplayGroup::launchQueryArgs(m_DisplayGroup, false);
}

void Session::finishDisplayGroupLaunch(const QString& launchResponse, bool resumingSession)
{
    if (m_DisplayGroup.role != DisplayGroup::Role::Leader || resumingSession ||
        m_DisplayGroup.requestedHeads < 2) {
        return;
    }
    m_DisplayGroup.groupId = DisplayGroup::groupIdFromLaunchResponse(launchResponse);
    if (m_DisplayGroup.groupId.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: host launched without a group id; streaming one display");
    }
}

bool Session::startDisplayGroupMemberLink()
{
    if (!isDisplayGroupMember() || m_DisplayGroupLink != nullptr) {
        return true;
    }

    QString error;
    if (!DisplayGroup::validateMemberOptions(m_DisplayGroup, &error)) {
        emit displayLaunchError(tr("Invalid display group arguments: %1").arg(error));
        return false;
    }

    auto link = std::make_unique<DisplayGroup::MemberLink>();
    if (!link->start(m_DisplayGroup.ipcName, m_DisplayGroup.head,
                     [] { pushDisplayGroupEvent(k_SdlCodeDisplayGroupEnded); })) {
        emit displayLaunchError(tr("The Moonlight window that started this display group is no "
                                   "longer running."));
        return false;
    }

    // The host has one keyboard state, so a member's keys go out on the
    // leader's session. Mouse, touch and pen stay on this head's session.
    DisplayGroup::MemberLink* raw = link.get();
    KeyboardSink::setForwarder(
        [raw](short code, char action, char modifiers, char flags) {
            return raw->sendKey(code, action, modifiers, flags);
        },
        [raw](const char* text, unsigned int length) {
            return raw->sendText(QByteArray(text, int(length)));
        });
    m_DisplayGroupLink = std::move(link);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display group: member for head %d joined group %s",
                m_DisplayGroup.head, qPrintable(m_DisplayGroup.groupId));
    return true;
}

void Session::startDisplayGroupLeaderHub()
{
    if (m_DisplayGroup.role != DisplayGroup::Role::Leader || m_DisplayGroupHub != nullptr ||
        !DisplayGroup::isValidGroupId(m_DisplayGroup.groupId)) {
        return;
    }

    auto hub = std::make_unique<DisplayGroup::LeaderHub>();
    const bool started =
        hub->start([](short code, char action, char modifiers,
                      char flags) { return LiSendKeyboardEvent2(code, action, modifiers, flags); },
                   [](const QByteArray& utf8) {
                       return LiSendUtf8TextEvent(utf8.constData(), (unsigned int)utf8.size());
                   },
                   DisplayGroup::createProcessMemberLauncher());
    if (!started) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: failed to start the member hub; streaming one display");
        return;
    }

    m_DisplayGroupMembersLaunched = false;
    m_DisplayGroupFallbackDeadline = SDL_GetTicks() + k_DisplayGroupLayoutTimeoutMs;
    m_DisplayGroupHub = std::move(hub);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display group %s: leader ready",
                qPrintable(m_DisplayGroup.groupId));
}

void Session::clDisplayGroup(const DISPLAY_GROUP_INFO* info)
{
    Session* session = s_ActiveSession;
    if (session == nullptr || info == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(session->m_DisplayGroupLayoutLock);
        session->m_PendingDisplayGroupLayout = DisplayGroup::layoutFromInfo(*info);
    }
    pushDisplayGroupEvent(k_SdlCodeDisplayGroupLayout);
}

int Session::displayGroupWaitTimeoutMs() const
{
    if (m_DisplayGroupHub == nullptr || m_DisplayGroupMembersLaunched) {
        return -1;
    }
    return qMax(0, (int)(Sint32)(m_DisplayGroupFallbackDeadline - SDL_GetTicks()));
}

void Session::serviceDisplayGroup()
{
    if (m_DisplayGroup.role == DisplayGroup::Role::None) {
        return;
    }

    std::optional<DisplayGroup::Layout> layout;
    {
        std::lock_guard<std::mutex> lock(m_DisplayGroupLayoutLock);
        layout.swap(m_PendingDisplayGroupLayout);
    }

    if (layout) {
        for (const DisplayGroup::Head& head : std::as_const(layout->heads)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Display group head %d: %dx%d at (%d,%d) %d mHz, admission %d reason %d",
                        head.index, head.width, head.height, head.x, head.y, head.refreshMilliHz,
                        head.admission, head.reason);
        }
    }

    if (m_DisplayGroupHub == nullptr || m_DisplayGroupMembersLaunched || m_Window == nullptr) {
        return;
    }

    if (layout && layout->groupId != m_DisplayGroup.groupId) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: ignoring layout for another group %s",
                    qPrintable(layout->groupId));
        layout.reset();
    }
    if (!layout && SDL_TICKS_PASSED(SDL_GetTicks(), m_DisplayGroupFallbackDeadline)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: no layout from the host; placing %d heads by default",
                    m_DisplayGroup.requestedHeads);
        layout =
            DisplayGroup::fallbackLayout(m_DisplayGroup.groupId, m_DisplayGroup.requestedHeads);
    }
    if (layout) {
        launchDisplayGroupMembers(*layout);
    }
}

void Session::launchDisplayGroupMembers(const DisplayGroup::Layout& layout)
{
    QVector<DisplayGroup::ClientDisplay> displays;
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) == 0) {
            displays.append({ i, QRect(bounds.x, bounds.y, bounds.w, bounds.h) });
        }
    }
    const int leaderDisplay = SDL_GetWindowDisplayIndex(m_Window);
    const QVector<DisplayGroup::MemberPlan> plans = DisplayGroup::planMembers(
        layout, displays, leaderDisplay, m_StreamConfig.width, m_StreamConfig.height);

    QVector<QPair<int, QStringList>> members;
    for (const DisplayGroup::MemberPlan& plan : plans) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Display group: head %d -> client display %d %s at %dx%d", plan.head,
                    plan.displayIndex, plan.fullscreen ? "fullscreen" : "windowed", plan.width,
                    plan.height);
        members.append({ plan.head, DisplayGroup::memberArguments(m_Computer->uuid, m_App.name,
                                                                  m_DisplayGroup.groupId,
                                                                  m_DisplayGroupHub->serverName(),
                                                                  m_StreamConfig.fps, plan) });
    }
    m_DisplayGroupHub->launchMembers(members);
    m_DisplayGroupMembersLaunched = true;
}

void Session::stopDisplayGroup(const char* reason)
{
    if (m_DisplayGroupHub != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Display group %s: ending (%s)",
                    qPrintable(m_DisplayGroup.groupId), reason);
        m_DisplayGroupHub->stop(k_DisplayGroupMemberGraceMs);
        m_DisplayGroupHub.reset();
    }
    if (m_DisplayGroup.role == DisplayGroup::Role::Leader) {
        // The host ends the group with the leader's stream; later resumes are N=1.
        m_DisplayGroup.groupId.clear();
    }
    if (m_DisplayGroupLink != nullptr) {
        KeyboardSink::clearForwarder();
        m_DisplayGroupLink->stop();
        m_DisplayGroupLink.reset();
    }
}
