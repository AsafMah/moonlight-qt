#include "../../app/streaming/video/overlaymenupanel.h"
#include "../../app/streaming/stats/statssnapshot.h"
#include <QGuiApplication>
#include <QReadLocker>
#include <QReadWriteLock>
#include <QScreen>
#include <QTest>
#include <atomic>
#include <cmath>
#include <cstdio>

using Uint32 = unsigned int;
constexpr Uint32 SDL_WINDOW_FULLSCREEN = 1;
constexpr Uint32 SDL_WINDOW_FULLSCREEN_DESKTOP = 0x1001;
constexpr Uint32 SDL_WINDOW_BORDERLESS = 0x10;
constexpr Uint32 SDL_WINDOW_MAXIMIZED = 0x80;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0, SDL_RENDER_DEVICE_RESET = 1;
constexpr int SDL_WINDOWEVENT = 2, SDL_WINDOWEVENT_SIZE_CHANGED = 3, SDL_MOUSEMOTION = 4;
constexpr int SDL_TRUE = 1, SDL_FALSE = 0;
#define SDL_VERSION_ATLEAST(a, b, c) 1

struct SDL_Window
{
    Uint32 flags = 0;
    QRect bounds{ 100, 200, 900, 600 };
    QRect display{ 0, 0, 1920, 1080 };
    int calls = 0, failCall = 0;
};
struct SDL_Rect
{
    int x, y, w, h;
};
struct SDL_Event
{
    int type = 0;
    struct
    {
        unsigned int windowID = 0;
        int event = 0, data1 = 0, data2 = 0;
    } window;
};
static SDL_Window* currentWindow;
static int resetEvents = 0;
static int warnings = 0;
static void SDL_LogWarn(int, const char*, ...)
{
    ++warnings;
}
static void SDL_LogInfo(int, const char*, ...) {}
static const char* SDL_GetError()
{
    return "simulated mode failure";
}
static Uint32 SDL_GetWindowFlags(SDL_Window* w)
{
    return w->flags;
}
static void SDL_GetWindowPosition(SDL_Window* w, int* x, int* y)
{
    *x = w->bounds.x();
    *y = w->bounds.y();
}
static void SDL_GetWindowSize(SDL_Window* w, int* x, int* y)
{
    *x = w->bounds.width();
    *y = w->bounds.height();
}
static void SDL_SetWindowSize(SDL_Window* w, int x, int y)
{
    w->bounds.setSize(QSize(x, y));
}
static void SDL_SetWindowPosition(SDL_Window* w, int x, int y)
{
    w->bounds.moveTopLeft(QPoint(x, y));
}
static void SDL_SetWindowBordered(SDL_Window* w, int bordered)
{
    if (bordered)
        w->flags &= ~SDL_WINDOW_BORDERLESS;
    else
        w->flags |= SDL_WINDOW_BORDERLESS;
}
static void SDL_MaximizeWindow(SDL_Window* w)
{
    w->flags |= SDL_WINDOW_MAXIMIZED;
}
static int SDL_SetWindowFullscreen(SDL_Window* w, Uint32 flags)
{
    if (++w->calls == w->failCall)
        return -1;
    w->flags &= ~(SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS | SDL_WINDOW_MAXIMIZED);
    w->flags |= flags;
    if (flags) {
        w->bounds = w->display;
        if (flags == SDL_WINDOW_FULLSCREEN_DESKTOP)
            w->flags |= SDL_WINDOW_BORDERLESS;
    }
    return 0;
}
static int SDL_GetWindowDisplayIndex(SDL_Window*)
{
    return 0;
}
static const char* videoDriver = "windows";
static const char* SDL_GetCurrentVideoDriver()
{
    return videoDriver;
}
static int SDL_GetDisplayBounds(int, SDL_Rect* r)
{
    auto b = currentWindow->display;
    *r = { b.x(), b.y(), b.width(), b.height() };
    return 0;
}
static void SDL_LockMutex(void*) {}
static void SDL_UnlockMutex(void*) {}
static int SDL_PushEvent(SDL_Event*)
{
    ++resetEvents;
    return 1;
}
static void pushEventOrWarn(SDL_Event& event)
{
    SDL_PushEvent(&event);
}
static unsigned int SDL_GetWindowID(SDL_Window*)
{
    return 1;
}
static void SDL_Delay(int) {}
static void SDL_FlushEvent(int) {}
static Uint32 SDL_GetTicks()
{
    return 1000;
}
static uint64_t LiGetMicroseconds()
{
    return 100000000;
}
static bool LiGetCurrentHostDisplayHdrMode()
{
    return true;
}
#define SDL_TICKS_PASSED(a, b) ((int)((b) - (a)) <= 0)
namespace WMUtils {
static bool desktop = true;
static bool isRunningDesktopEnvironment()
{
    return desktop;
}
}
struct QtShell
{
    Qt::WindowStates states;
    QRect bounds{ 100, 200, 900, 600 };
    bool reject = false;
    Qt::WindowStates windowStates() const { return states; }
    void showFullScreen()
    {
        if (!reject)
            states = Qt::WindowFullScreen;
    }
    void showNormal()
    {
        if (!reject)
            states = Qt::WindowNoState;
    }
    void showMaximized() { states = Qt::WindowMaximized; }
    QRect geometry() const { return bounds; }
    void setGeometry(QRect r) { bounds = r; }
    QScreen* screen() const { return QGuiApplication::primaryScreen(); }
    Qt::WindowFlags flags() const { return Qt::FramelessWindowHint; }
};
struct Input
{
    bool captured = false;
    bool desktop = false, cursor = false, region = false, systemKeys = false;
    int keyboardUpdates = 0, pointerUpdates = 0, captures = 0;
    void updateKeyboardGrabState() { ++keyboardUpdates; }
    void updatePointerRegionLock() { ++pointerUpdates; }
    void setCaptureActive(bool value)
    {
        captured = value;
        ++captures;
    }
    bool isAbsoluteMouseMode() const { return desktop; }
    bool isCapturedCursorVisible() const { return cursor; }
    bool isPointerRegionLockEnabled() const { return region; }
    bool isSystemKeyCaptureEnabledForWindow() const { return systemKeys; }
    void setAbsoluteMouseMode(bool value) { desktop = value; }
    void raiseAllKeys() {}
};
class Session
{
public:
    using WindowMode = OverlayMenuPanel::WindowMode;
    SDL_Window* m_Window;
    QtShell* m_WaylandStreamWindow = nullptr;
    Input input;
    Input* m_InputHandler = &input;
    int* m_VideoDecoder = new int;
    void* m_DecoderLock = nullptr;
    Uint32 m_FullScreenFlag = SDL_WINDOW_FULLSCREEN_DESKTOP;
    QRect m_WindowedBounds;
    bool m_WindowedMaximized = false;
    OverlayMenuPanel* m_MenuPanel = nullptr;
    bool m_DeferCaptureRestore = false, m_WasCapturedBeforeMenu = false;
    int overlaySyncs = 0, toasts = 0;
    explicit Session(SDL_Window& window) : m_Window(&window) { currentWindow = &window; }
    ~Session() { delete m_VideoDecoder; }
    WindowMode streamingWindowMode() const;
    bool supportsStreamingWindowMode(WindowMode mode) const;
    bool setStreamingWindowMode(WindowMode mode);
    void toggleFullscreen();
    bool isStreamingWindowFullscreen() const;
    bool isStreamingWindowBorderlessOnDisplay() const;
    bool isStreamingWindowMinimized() const { return false; }
    bool isStreamingWindowVisible() const { return true; }
    void updateWindowModeMenuState();
    bool handleWindowModeMenuAction(OverlayMenuPanel::MenuAction action);
    void restoreCaptureAfterWindowModeChange();
    void dispatchQtMenuAction(OverlayMenuPanel::MenuAction action);
    void processQtOverlayEvents() {}
    void syncQtOverlayWindowsWithSdlWindowState() { ++overlaySyncs; }
    void showStreamingToast(const QString&, int) { ++toasts; }
    QString tr(const char* text) const { return QString::fromUtf8(text); }
    bool handleOverlayMenuOptionAction(OverlayMenuPanel::MenuAction) { return false; }
    bool handleStatsOverlayMenuAction(OverlayMenuPanel::MenuAction) { return false; }
    bool handleLiveMenuAction(OverlayMenuPanel::MenuAction);
    void finishOverlayMenuInput();
    void updateAudioMuteState();
    void updateLiveMenuState();
    std::optional<bool> m_PendingDesktopMouseMode;
    bool m_ManualAudioMuted = false, m_StreamInputFocused = true;
    std::atomic<bool> m_AudioMuted{ false };
    bool m_SuppressConnectionErrorDialog = false, m_PendingStreamQuality = false;
    struct
    {
        bool hostSupported = true, oneShot = false;
        Uint32 requestTicks = 0, lastAttemptTicks = 0;
    } m_ResolutionMatch;
    int matchRequests = 0;
    bool requestWindowResolutionMatchNow()
    {
        ++matchRequests;
        return true;
    }
    void scheduleWindowResolutionMatch() {}
    struct
    {
        StatsSnapshot value;
        StatsSnapshot snapshot(uint64_t) const { return value; }
    } m_StatsModel;
    struct
    {
        QReadWriteLock lock;
        QString name = QStringLiteral("Fixture host");
        QString appVersion = QStringLiteral("7.0-fixture");
    } computer;
    decltype(computer)* m_Computer = &computer;
    struct
    {
        QString name = QStringLiteral("Desktop");
    } m_App;
    struct
    {
        int width = 1920, height = 1080, fps = 60;
    } m_StreamConfig;
    struct
    {
        int channelCount = 2;
    } m_ActiveAudioConfig;
    int m_ActiveVideoWidth = 1920, m_ActiveVideoHeight = 1080;
    std::vector<std::pair<QString, QString>> m_ConnectionMenuRows;
    std::vector<std::pair<QString, QString>> sessionStatsMenuRows() const;
    void setupSessionStatsMenu();
    void updateConnectionMenuInfo();
    struct Preferences
    {
        void setOverlayMenuPosition(int) {}
        void save() {}
        bool muteOnFocusLoss = false, resolutionMatchWindow = false;
    } preferences;
    Preferences* m_Preferences = &preferences;
    void reloadOverlayMenuOptions() {}
};
static std::optional<int> menuPlacementForAction(OverlayMenuPanel::MenuAction)
{
    return std::nullopt;
}
struct OverlayMenuSettings
{
    void resetDraggedPosition() {}
};
#include "production_methods.inc"

static void require(bool ok, const char* message)
{
    if (!ok)
        qFatal("%s", message);
}
using Mode = Session::WindowMode;
using Action = OverlayMenuPanel::MenuAction;
static void transitions()
{
    for (Mode start : { Mode::Windowed, Mode::Borderless, Mode::Fullscreen }) {
        for (Mode target : { Mode::Windowed, Mode::Borderless, Mode::Fullscreen }) {
            SDL_Window window;
            Session session(window);
            const QRect bounds = window.bounds;
            require(session.setStreamingWindowMode(start), "cannot seed mode");
            int calls = window.calls;
            const int resets = resetEvents;
            require(session.setStreamingWindowMode(target), "directed transition failed");
            require(session.streamingWindowMode() == target, "mode not truthful");
            if (start == target)
                require(window.calls == calls && resetEvents == resets,
                        "current mode was not a no-op");
            require(session.setStreamingWindowMode(Mode::Windowed), "return failed");
            require(window.bounds == bounds, "windowed bounds were not restored");
            session.toggleFullscreen();
            require(session.streamingWindowMode() ==
                        (target == Mode::Windowed
                             ? (start == Mode::Fullscreen ? start : Mode::Borderless)
                             : target),
                    "shortcut lost last flavor");
            session.toggleFullscreen();
            require(session.streamingWindowMode() == Mode::Windowed && window.bounds == bounds,
                    "shortcut did not restore bounds");
        }
    }
    SDL_Window w;
    Session s(w);
    w.failCall = 1;
    require(!s.setStreamingWindowMode(Mode::Fullscreen) &&
                s.streamingWindowMode() == Mode::Windowed && s.toasts > 0,
            "failed entry lied");
    require(resetEvents > 0, "deleted decoder was not scheduled for recreation");
    w.failCall = 0;
    require(s.setStreamingWindowMode(Mode::Borderless), "seed borderless");
    w.failCall = w.calls + 1;
    require(!s.setStreamingWindowMode(Mode::Fullscreen) &&
                s.streamingWindowMode() == Mode::Borderless,
            "failed flavor change lied");
    w.failCall = w.calls + 1;
    require(!s.setStreamingWindowMode(Mode::Windowed) &&
                s.streamingWindowMode() == Mode::Borderless,
            "failed exit lied");
    w.failCall = 0;
    require(s.setStreamingWindowMode(Mode::Windowed), "return to windowed failed");
    w.bounds = QRect(-800, 100, 700, 500);
    require(s.setStreamingWindowMode(Mode::Borderless) &&
                s.setStreamingWindowMode(Mode::Fullscreen) &&
                s.setStreamingWindowMode(Mode::Windowed) && w.bounds == QRect(-800, 100, 700, 500),
            "later user-moved window bounds were not remembered");
    WMUtils::desktop = false;
    require(!s.supportsStreamingWindowMode(Mode::Borderless), "WM-less borderless advertised");
    WMUtils::desktop = true;
    videoDriver = "wayland";
    require(!s.supportsStreamingWindowMode(Mode::Fullscreen), "SDL Wayland exclusive advertised");
    videoDriver = "windows";
    QtShell shell;
    s.m_WaylandStreamWindow = &shell;
    require(!s.supportsStreamingWindowMode(Mode::Fullscreen), "Wayland exclusive advertised");
    require(!s.setStreamingWindowMode(Mode::Fullscreen) &&
                s.streamingWindowMode() == Mode::Windowed,
            "unsupported mode changed shell");
    require(s.setStreamingWindowMode(Mode::Borderless) &&
                s.streamingWindowMode() == Mode::Borderless,
            "Qt fullscreen state not authoritative");
    require(s.setStreamingWindowMode(Mode::Windowed) && shell.bounds == QRect(100, 200, 900, 600),
            "Qt bounds restore failed");
    shell.reject = true;
    require(!s.setStreamingWindowMode(Mode::Borderless) &&
                s.streamingWindowMode() == Mode::Windowed,
            "rejected Qt request lied");
}
static Action actionForMode(Mode mode)
{
    return mode == Mode::Windowed     ? Action::SetWindowModeWindowed
           : mode == Mode::Borderless ? Action::SetWindowModeBorderless
                                      : Action::SetWindowModeFullscreen;
}
static void menu()
{
    SDL_Window w;
    Session s(w);
    OverlayMenuPanel panel(nullptr);
    s.m_MenuPanel = &panel;
    s.setupSessionStatsMenu();
    s.updateConnectionMenuInfo();
    require(s.m_ConnectionMenuRows.size() == 11 &&
                s.m_ConnectionMenuRows.back().second == QStringLiteral("Not reported"),
            "actual connection facts must retain all 11 fields and not infer an encoder");
    require(s.sessionStatsMenuRows().size() == 9,
            "actual inline provider must retain all nine session totals");
    panel.setActionCallback([&](Action action) { s.dispatchQtMenuAction(action); });
    panel.setCloseCallback([&] { s.finishOverlayMenuInput(); });
    bool gamepads = false;
    const auto select = [&](Mode target, bool succeeds = true) {
        const Mode before = s.streamingWindowMode();
        const bool changesMode = s.streamingWindowMode() != target;
        const int syncsBefore = s.overlaySyncs;
        s.updateWindowModeMenuState();
        panel.showWithPlacement(OverlayMenu::Placement::Center, 0, 0, 1920, 1080, std::nullopt,
                                std::nullopt);
        panel.showTab(OverlayMenuPanel::MenuTab::Display);
        int row = panel.rowOfAction(Action::OpenWindowMode);
        require(row >= 0, "Window Mode missing on Display");
        QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.rowGeometry(row).center());
        QTest::qWait(250);
        const Action action = actionForMode(target);
        row = panel.rowOfAction(action);
        require(row >= 0, "explicit mode missing");
        const int currentRow = panel.rowOfAction(actionForMode(s.streamingWindowMode()));
        require(panel.rowDetail(currentRow) == QString::fromUtf8("\342\234\223"),
                "current state not marked");
        // Rebuilding for device changes must preserve the mode and sublevel.
        gamepads = !gamepads;
        panel.setGamepadHints(gamepads, GamepadUiStyleXbox, false, QStringLiteral("test"));
        require(panel.rowOfAction(action) >= 0 &&
                    panel.rowDetail(currentRow) == QString::fromUtf8("\342\234\223"),
                "menu rebuild lost current mode");
        s.m_WasCapturedBeforeMenu = true;
        s.input.captured = false;
        const bool detached = panel.isDetached();
        QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.rowGeometry(row).center());
        require(s.streamingWindowMode() == (succeeds ? target : before) && s.input.captured &&
                    !s.m_DeferCaptureRestore,
                "menu dispatch/capture failed");
        require(panel.isDetached() == detached && (!changesMode || s.overlaySyncs > syncsBefore),
                "attachment/sync lost");
        QTest::qWait(450);
    };
    for (bool detached : { false, true }) {
        panel.setDetached(detached);
        for (Mode start : { Mode::Windowed, Mode::Borderless, Mode::Fullscreen }) {
            for (Mode target : { Mode::Windowed, Mode::Borderless, Mode::Fullscreen }) {
                require(s.setStreamingWindowMode(start), "seed menu mode failed");
                select(target);
            }
        }
    }
    require(s.setStreamingWindowMode(Mode::Windowed), "seed failed menu entry");
    w.failCall = w.calls + 1;
    select(Mode::Fullscreen, false);
    w.failCall = 0;
    QtShell shell;
    s.m_WaylandStreamWindow = &shell;
    s.updateWindowModeMenuState();
    panel.showWithPlacement(OverlayMenu::Placement::Center, 0, 0, 1920, 1080, std::nullopt,
                            std::nullopt);
    panel.showLevel(Action::OpenWindowMode);
    const int disabled = panel.rowOfAction(Action::SetWindowModeFullscreen);
    require(panel.rowDetail(disabled) == QStringLiteral("Not supported"),
            "unsupported mode not explained");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.rowGeometry(disabled).center());
    require(s.streamingWindowMode() == Mode::Windowed && panel.isMenuVisible(),
            "disabled row dispatched");
    panel.closeMenu();
    QTest::qWait(450);
}

static void liveControls()
{
    SDL_Window w;
    Session s(w);
    OverlayMenuPanel panel(nullptr);
    s.m_MenuPanel = &panel;
    s.setupSessionStatsMenu();
    s.updateConnectionMenuInfo();
    panel.setActionCallback([&](Action action) { s.dispatchQtMenuAction(action); });
    panel.setCloseCallback([&] { s.finishOverlayMenuInput(); });
    const auto open = [&](OverlayMenuPanel::MenuTab tab, bool captured) {
        s.m_WasCapturedBeforeMenu = captured;
        s.input.captured = false;
        s.updateLiveMenuState();
        panel.showWithPlacement(OverlayMenu::Placement::Center, 0, 0, 1920, 1080, std::nullopt,
                                std::nullopt);
        panel.showTab(tab);
        QTest::qWait(160);
    };
    const auto click = [&](Action action) {
        const int row = panel.rowOfAction(action);
        require(row >= 0, "live control absent from canonical tab");
        QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.rowGeometry(row).center());
    };
    for (bool detached : { false, true }) {
        panel.setDetached(detached);
        for (bool captured : { false, true }) {
            for (bool desktop : { true, false }) {
                open(OverlayMenuPanel::MenuTab::Input, captured);
                const bool previous = s.input.desktop;
                click(Action::OpenMouseMode);
                QTest::qWait(160);
                click(desktop ? Action::SetMouseModeDesktop : Action::SetMouseModeGame);
                require(panel.isMenuVisible() && !s.input.captured && s.input.desktop == previous &&
                            s.m_PendingDesktopMouseMode == desktop,
                        "real mode row did not defer change while menu owns pointer");
                panel.closeMenu();
                QTest::qWait(400);
                require(s.input.desktop == desktop && s.input.captured == captured &&
                            panel.isDetached() == detached,
                        "real close failed to apply pending mode with original capture/attachment");
            }
            open(OverlayMenuPanel::MenuTab::Input, captured);
            click(Action::UngrabInput);
            if (captured) {
                require(!s.m_WasCapturedBeforeMenu && !s.input.captured,
                        "Release mouse did not clear restoration synchronously");
            } else {
                require(panel.isMenuVisible(), "already free Release mouse was not disabled");
                panel.closeMenu();
            }
            QTest::qWait(400);
            require(!s.input.captured, "real close animation recaptured after Release mouse");
        }
        open(OverlayMenuPanel::MenuTab::Stream, true);
        click(Action::ToggleAudioMute);
        require(s.m_ManualAudioMuted && s.m_AudioMuted.load(), "real audio row did not mute");
        panel.closeMenu();
        QTest::qWait(400);
        open(OverlayMenuPanel::MenuTab::Quick, true);
        click(Action::ToggleAudioMute);
        require(!s.m_ManualAudioMuted && !s.m_AudioMuted.load(),
                "Quick pin did not dispatch against the canonical manual mute");
        panel.closeMenu();
        QTest::qWait(400);
        open(OverlayMenuPanel::MenuTab::Session, true);
        panel.gamepadMoveDown();
        panel.gamepadSelect();
        require(s.preferences.muteOnFocusLoss,
                "controller focus did not skip inline facts/totals and reach focus-mute control");
        s.m_StreamInputFocused = false;
        s.updateAudioMuteState();
        require(s.m_AudioMuted.load(), "actual Session policy failed to mute when unfocused");
        s.m_StreamInputFocused = true;
        s.updateAudioMuteState();
        require(!s.m_AudioMuted.load(), "focus regain left effective mute stuck");
        s.handleLiveMenuAction(Action::ToggleMuteOnFocusLoss);
        panel.closeMenu();
        QTest::qWait(400);
        open(OverlayMenuPanel::MenuTab::Display, false);
        const int before = s.matchRequests;
        click(Action::MatchResolutionNow);
        require(s.matchRequests == before + 1 && !s.preferences.resolutionMatchWindow,
                "real Match now changed automatic policy or missed dispatch");
        panel.closeMenu();
        QTest::qWait(400);
    }
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    transitions();
    menu();
    liveControls();
    std::puts("PASS live window modes: six transitions, no-op, failures, bounds, shortcut, native "
              "restrictions, menu dispatch/capture");
    std::puts("PASS real Phase 1 menu dispatch: pending Game/Desktop, Release mouse, manual mute "
              "and Quick parity, Match now; attached/detached and captured/free close paths");
}
