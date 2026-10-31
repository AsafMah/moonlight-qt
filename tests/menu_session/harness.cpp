#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using QString = const char*;
namespace Qt {
enum KeyboardModifier
{
    NoModifier = 0,
    ControlModifier = 1,
    AltModifier = 2,
    ShiftModifier = 4,
    MetaModifier = 8
};
using KeyboardModifiers = int;
}
static constexpr int KMOD_CTRL = 1, KMOD_ALT = 2, KMOD_SHIFT = 4, KMOD_GUI = 8;
static constexpr int SDL_KEYDOWN = 1, SDL_KEYUP = 2;
static constexpr int SDL_USEREVENT = 24;
static constexpr int SDL_CODE_LIVE_RESOLUTION_FIRST_FRAME = 113;
struct SDL_UserEvent
{
    int code = 0;
    void* data1 = nullptr;
};
struct SDL_Event
{
    int type = 0;
    SDL_UserEvent user;
};
struct SDL_KeyboardEvent
{
    int type = 0;
    struct
    {
        int mod = 0;
        int scancode = 0;
    } keysym;
};
static QString tr(const char* text)
{
    return text;
}
static constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
static void SDL_LogInfo(int, const char*, ...) {}
static void SDL_LogWarn(int, const char*, ...) {}

struct OverlayMenuPanel
{
    enum class MenuAction
    {
        UngrabInput,
        SetMouseModeGame,
        SetMouseModeDesktop,
        ToggleAudioMute,
        ToggleMuteOnFocusLoss,
        ToggleResolutionMatch,
        MatchResolutionNow,
        RetryLiveResolution,
        ReconnectStreamQuality,
        ToggleCursorHide,
        TogglePointerRegionLock,
        Other
    };
    bool capturing = false;
    int scan = -1, modifiers = -1, calls = 0;
    bool pressed = false;
    bool isCapturingShortcut() const { return capturing; }
    void handleShortcutKey(int key, Qt::KeyboardModifiers flags, bool down)
    {
        scan = key;
        modifiers = flags;
        pressed = down;
        ++calls;
        if (!down)
            capturing = false;
    }
};
struct Preferences
{
    bool muteOnFocusLoss = false;
    bool resolutionMatchWindow = false;
    int saves = 0;
    void save() { ++saves; }
};
struct ResolutionMatchState
{
    bool explicitOverride = false;
};
struct SdlInputHandler
{
    bool m_AbsoluteMouseMode = false;
    bool captured = false;
    int cursorSyncs = 0;
    bool isCaptureActive() { return captured; }
    bool isAbsoluteMouseMode() { return m_AbsoluteMouseMode; }
    void setCaptureActive(bool value) { captured = value; }
    void raiseAllKeys() {}
    void synchronizeLocalCursorMode() { ++cursorSyncs; }
    void setAbsoluteMouseMode(bool value);
};
struct Session
{
    Preferences preferences;
    Preferences* m_Preferences = &preferences;
    SdlInputHandler input;
    SdlInputHandler* m_InputHandler = &input;
    bool m_WasCapturedBeforeMenu = true;
    bool m_DeferCaptureRestore = false;
    std::optional<bool> m_PendingDesktopMouseMode;
    ResolutionMatchState m_ResolutionMatch;
    bool m_HasExplicitStreamQualityOverride = false;
    std::atomic<bool> m_HasReceivedVideo{ false };
    std::atomic<bool> m_LiveResolutionAwaitingFirstFrame{ false };
    std::atomic<std::uint32_t> m_ConnectionGeneration{ 0 };
    std::vector<SDL_Event> pushedEvents;
    bool m_ManualAudioMuted = false;
    bool m_StreamInputFocused = true;
    std::atomic<bool> m_AudioMuted{ false };
    bool matchAvailable = true;
    int matchRequests = 0;
    bool visible = true;
    OverlayMenuPanel panel;
    OverlayMenuPanel* m_MenuPanel = &panel;
    static Session* s_ActiveSession;
    void showStreamingToast(QString, int) {}
    void updateLiveMenuState() {}
    void scheduleWindowResolutionMatch() {}
    static void markVideoReceived();
    void restoreResolutionMatchOverride();
    bool requestWindowResolutionMatchNow()
    {
        ++matchRequests;
        return matchAvailable;
    }
    void retryFailedLiveResolution(bool) {}
    bool isStreamingWindowVisible() { return visible; }
    bool handleLiveMenuAction(OverlayMenuPanel::MenuAction);
    void finishOverlayMenuInput();
    void updateAudioMuteState();
    bool handleOverlayMenuShortcutEvent(const SDL_KeyboardEvent&);
};
Session* Session::s_ActiveSession = nullptr;

static void pushEventOrWarn(SDL_Event& event)
{
    if (Session::s_ActiveSession) {
        Session::s_ActiveSession->pushedEvents.push_back(event);
    }
}

// METHODS

static bool require(bool value, const char* message)
{
    if (!value)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}

int main()
{
    bool ok = true;
    using Action = OverlayMenuPanel::MenuAction;
    {
        Session s;
        SDL_KeyboardEvent key;
        key.type = SDL_KEYDOWN;
        key.keysym.scancode = 65;
        key.keysym.mod = KMOD_CTRL | KMOD_ALT | KMOD_SHIFT | KMOD_GUI;
        ok &= require(!s.handleOverlayMenuShortcutEvent(key),
                      "ordinary key must not be stolen without shortcut capture");
        s.panel.capturing = true;
        ok &= require(s.handleOverlayMenuShortcutEvent(key) && s.panel.scan == 65 &&
                          s.panel.modifiers == (Qt::ControlModifier | Qt::AltModifier |
                                                Qt::ShiftModifier | Qt::MetaModifier) &&
                          s.panel.pressed,
                      "editor keydown must route SDL scancode and translated modifiers");
        key.type = SDL_KEYUP;
        ok &= require(s.handleOverlayMenuShortcutEvent(key) && !s.panel.pressed &&
                          !s.panel.capturing && s.panel.calls == 2,
                      "editor key release must be swallowed until capture drains");
    }
    for (bool captured : { false, true }) {
        Session s;
        s.m_WasCapturedBeforeMenu = captured;
        s.handleLiveMenuAction(Action::UngrabInput);
        s.finishOverlayMenuInput();
        ok &= require(!s.input.captured && !s.m_WasCapturedBeforeMenu,
                      "Release mouse must stay free after animation close");
        for (bool desktop : { true, false, true }) {
            s.m_WasCapturedBeforeMenu = captured;
            s.input.captured = false;
            const bool previous = s.input.isAbsoluteMouseMode();
            s.handleLiveMenuAction(desktop ? Action::SetMouseModeDesktop
                                           : Action::SetMouseModeGame);
            ok &= require(!s.input.captured && s.input.isAbsoluteMouseMode() == previous,
                          "mouse mode must remain pending while menu is open");
            s.finishOverlayMenuInput();
            ok &= require(s.input.isAbsoluteMouseMode() == desktop &&
                              s.input.captured == captured && !s.m_PendingDesktopMouseMode,
                          "close must apply mode and preserve pre-menu capture");
        }
    }
    {
        Session s;
        s.input.setAbsoluteMouseMode(true);
        s.input.captured = true;
        s.input.setAbsoluteMouseMode(false);
        ok &= require(s.input.captured && !s.input.isAbsoluteMouseMode(),
                      "explicit input setter must preserve capture outside menu");
        s.handleLiveMenuAction(Action::SetMouseModeDesktop);
        ok &= require(!s.handleLiveMenuAction(Action::ToggleCursorHide),
                      "pending Desktop must allow cursor control");
        s.handleLiveMenuAction(Action::SetMouseModeGame);
        ok &= require(s.handleLiveMenuAction(Action::ToggleCursorHide) &&
                          s.handleLiveMenuAction(Action::TogglePointerRegionLock),
                      "Game mode must intercept Desktop-only actions");
    }
    {
        Session s;
        s.handleLiveMenuAction(Action::ToggleAudioMute);
        s.m_StreamInputFocused = false;
        s.updateAudioMuteState();
        s.m_StreamInputFocused = true;
        s.updateAudioMuteState();
        ok &= require(s.m_AudioMuted.load(), "manual mute must survive focus regain");
        s.handleLiveMenuAction(Action::ToggleAudioMute);
        s.handleLiveMenuAction(Action::ToggleMuteOnFocusLoss);
        s.m_StreamInputFocused = false;
        s.updateAudioMuteState();
        ok &= require(s.m_AudioMuted.load(), "focus policy must mute on focus loss");
        s.handleLiveMenuAction(Action::ToggleMuteOnFocusLoss);
        ok &= require(!s.m_AudioMuted.load(), "turning focus policy off must unmute immediately");
    }
    {
        Session s;
        s.handleLiveMenuAction(Action::MatchResolutionNow);
        ok &= require(s.matchRequests == 1 && !s.preferences.resolutionMatchWindow &&
                          s.preferences.saves == 0,
                      "Match now dispatch must not alter saved automatic policy");
    }
    {
        Session s;
        Session::s_ActiveSession = &s;
        s.m_ConnectionGeneration = 42;
        s.m_LiveResolutionAwaitingFirstFrame = true;
        Session::markVideoReceived();
        ok &=
            require(s.m_HasReceivedVideo && !s.m_LiveResolutionAwaitingFirstFrame &&
                        s.pushedEvents.size() == 1 &&
                        s.pushedEvents.front().user.code == SDL_CODE_LIVE_RESOLUTION_FIRST_FRAME &&
                        (uintptr_t)s.pushedEvents.front().user.data1 == 42,
                    "first host frame must complete the live-resolution observation once");
        Session::markVideoReceived();
        ok &= require(s.pushedEvents.size() == 1,
                      "subsequent host frames must not duplicate first-frame confirmation");
        Session::s_ActiveSession = nullptr;
    }
    {
        Session s;
        s.m_HasExplicitStreamQualityOverride = true;
        s.m_ResolutionMatch.explicitOverride = true;
        s.handleLiveMenuAction(Action::ToggleResolutionMatch);
        s.restoreResolutionMatchOverride();
        ok &=
            require(!s.m_HasExplicitStreamQualityOverride && !s.m_ResolutionMatch.explicitOverride,
                    "re-enabling automatic matching must survive reconnect restoration");
    }
    {
        Session s;
        s.m_HasExplicitStreamQualityOverride = true;
        s.m_ResolutionMatch.explicitOverride = true;
        s.handleLiveMenuAction(Action::MatchResolutionNow);
        s.restoreResolutionMatchOverride();
        ok &=
            require(!s.m_HasExplicitStreamQualityOverride && !s.m_ResolutionMatch.explicitOverride,
                    "Match now must clear stale explicit ownership across reconnect");
    }
    return ok ? 0 : 1;
}
