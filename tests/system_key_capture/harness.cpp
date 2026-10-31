#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <functional>

#define SDL_VERSION_ATLEAST(a, b, c) 1
using Uint32 = unsigned int;
using qint64 = long long;
constexpr int SDL_TRUE = 1, SDL_FALSE = 0;
constexpr Uint32 SDL_WINDOW_INPUT_FOCUS = 1, SDL_WINDOW_FULLSCREEN = 2;
constexpr Uint32 SDL_WINDOW_BORDERLESS = 4, SDL_WINDOW_MINIMIZED = 8;
constexpr Uint32 SDL_WINDOW_FULLSCREEN_DESKTOP = SDL_WINDOW_FULLSCREEN | 16;
constexpr int SDL_ENABLE = 1, SDL_MOUSEMOTION = 1, SDL_SCANCODE_ESCAPE = 41, SDL_PRESSED = 1;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
void SDL_LogInfo(int, const char*, ...) {}
int warnings = 0;
void SDL_LogWarn(int, const char*, ...)
{
    ++warnings;
}
const char* SDL_GetError()
{
    return "fake display query failure";
}

// RELEASE_GESTURE

struct QRect
{
    int x = 0, y = 0, w = 0, h = 0;
    QRect() = default;
    QRect(int x, int y, int w, int h) : x(x), y(y), w(w), h(h) {}
    bool contains(const QRect& other) const
    {
        return w > 0 && h > 0 && other.w > 0 && other.h > 0 && x <= other.x && y <= other.y &&
               x + w >= other.x + other.w && y + h >= other.y + other.h;
    }
};
using SDL_Rect = QRect;
namespace Qt {
constexpr int FramelessWindowHint = 1, WindowFullScreen = 2, WindowMinimized = 4;
}
struct Flags
{
    int value = 0;
    bool testFlag(int flag) const { return (value & flag) != 0; }
};
struct QScreen
{
    QRect bounds{ 0, 0, 1280, 720 };
    QRect geometry() const { return bounds; }
};
struct QtWindow
{
    QScreen display;
    QScreen* output = &display;
    QRect bounds{ 0, 0, 1280, 720 };
    int style = Qt::FramelessWindowHint;
    Flags states;
    int flags() const { return style; }
    Flags windowStates() const { return states; }
    QScreen* screen() const { return output; }
    QRect geometry() const { return bounds; }
};

struct StreamingPreferences
{
    // CAPTURE_ENUM
    WindowMode windowMode = WM_FULLSCREEN_DESKTOP;
};

struct SDL_Window
{
    Uint32 flags = SDL_WINDOW_INPUT_FOCUS;
    bool keyboardGrab = false;
    QRect client{ 0, 0, 1920, 1080 };
    QRect display{ 0, 0, 1920, 1080 };
    bool displayQueryOk = true;
};
SDL_Window* currentWindow;
Uint32 SDL_GetWindowFlags(SDL_Window* window)
{
    return window->flags;
}
void SDL_SetWindowKeyboardGrab(SDL_Window* window, int grab)
{
    window->keyboardGrab = grab;
}
void SDL_GetWindowPosition(SDL_Window* w, int* x, int* y)
{
    *x = w->client.x;
    *y = w->client.y;
}
void SDL_GetWindowSize(SDL_Window* w, int* x, int* y)
{
    *x = w->client.w;
    *y = w->client.h;
}
int SDL_GetWindowDisplayIndex(SDL_Window*)
{
    return 0;
}
int SDL_GetDisplayBounds(int, SDL_Rect* bounds)
{
    *bounds = currentWindow->display;
    return currentWindow->displayQueryOk ? 0 : -1;
}
bool relativeMouseMode = true;
int SDL_SetRelativeMouseMode(int enabled)
{
    relativeMouseMode = enabled;
    return 0;
}
bool SDL_GetRelativeMouseMode()
{
    return relativeMouseMode;
}
void SDL_GetGlobalMouseState(int* x, int* y)
{
    *x = *y = 0;
}
int SDL_GetTicks()
{
    return 0;
}
int SDL_GetWindowID(SDL_Window*)
{
    return 1;
}
void* SDL_GetCursor()
{
    return nullptr;
}
void* SDL_GetDefaultCursor()
{
    return nullptr;
}
void SDL_SetCursor(void*) {}
void SDL_ShowCursor(int) {}
struct SDL_MouseMotionEvent
{
    int type, timestamp, windowID, x, y;
};
struct SDL_KeyboardEvent
{
    struct
    {
        int scancode;
    } keysym;
    int state;
};
class SdlInputHandler;
struct StatsWindow
{
    bool armed = false;
    std::function<void(bool)> editCallback;
    void setPointerEditMode(bool enabled) { armed = enabled; }
    void setEditModeCallback(std::function<void(bool)> cb) { editCallback = std::move(cb); }
};
struct MenuWindow
{
    bool editing = false, visible = false;
    void setPointerEditMode(bool enabled) { editing = enabled; }
    bool isMenuVisible() const { return visible; }
    void closeMenu() { visible = false; }
};
class Session
{
public:
    static Session* active;
    static Session* get() { return active; }
    SDL_Window* m_Window = nullptr;
    QtWindow* m_WaylandStreamWindow = nullptr;
    StreamingPreferences preferences;
    StreamingPreferences* m_Preferences = &preferences;
    SdlInputHandler* m_InputHandler = nullptr;
    StatsWindow stats;
    MenuWindow menu;
    StatsWindow* m_StatsOverlay = &stats;
    MenuWindow* m_MenuPanel = &menu;
    bool m_WasCapturedBeforeMenu = false, m_WasCapturedBeforeStatsEdit = false;
    bool isStreamingWindowVisible() const { return true; }
    const char* tr(const char* text) { return text; }
    void showStreamingToast(const char*, int) {}
    void wireOverlayCallbacks();
    bool isStreamingWindowFullscreen() const;
    bool isStreamingWindowBorderlessOnDisplay() const;
    bool isStreamingWindowMinimized() const
    {
        return m_WaylandStreamWindow
                   ? m_WaylandStreamWindow->windowStates().testFlag(Qt::WindowMinimized)
                   : m_Window && (m_Window->flags & SDL_WINDOW_MINIMIZED);
    }
};
Session* Session::active = nullptr;
// SESSION_METHODS
bool isStreamWindowFullscreen(SDL_Window*)
{
    return Session::get()->isStreamingWindowFullscreen();
}

class SdlInputHandler
{
public:
    SDL_Window window;
    SDL_Window* m_Window = &window;
    Session session;
    SdlInputHandler()
    {
        session.m_Window = &window;
        session.m_InputHandler = this;
        Session::active = &session;
        currentWindow = &window;
        relativeMouseMode = true;
    }
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode =
        StreamingPreferences::CSK_ALWAYS;
    bool& captured = relativeMouseMode;
    bool m_KeyboardCaptureActive = false;
    bool m_MouseHoldReleased = false, m_AbsoluteMouseMode = false, m_FakeMouseCaptureActive = false;
    void* m_RemoteCursor = nullptr;
    MouseReleaseGesture m_MouseReleaseGesture;
    std::function<void(bool)> m_MouseReleaseModeCallback;
    void applyCapturedCursorState() {}
    bool isMouseInVideoRegion(int, int) { return false; }
    void handleMouseMotionEvent(SDL_MouseMotionEvent*) {}
    void cancelNativeTouchpadContacts() {}
    void updatePointerRegionLock() {}
    bool isCaptureActive();
    bool hasInputFocus() const;
    bool isSystemKeyCaptureActive();
    bool isSystemKeyCaptureEnabledForWindow();
    void updateKeyboardGrabState();
    void toggle();
    void setCaptureActive(bool active);
    void holdReleaseMouse(bool held);
    void handleMouseReleaseHold(bool held);
    void applyMouseReleaseGesture(bool wasFree);
    bool cancelMouseHoldRelease();
    void notifyFocusLost();
    void setMouseReleaseModeCallback(std::function<void(bool)> cb)
    {
        m_MouseReleaseModeCallback = std::move(cb);
    }
    struct
    {
        int count() const { return 0; }
    } m_KeysDown;
    struct
    {
        void reset() {}
    } m_ShortcutMatcher;
    void raiseAllKeys() {}
};

// INPUT_METHODS
// OVERLAY_CALLBACKS

int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    SdlInputHandler input;
    if (std::strcmp(argv[1], "toggle") == 0) {
        input.updateKeyboardGrabState();
        check(input.isSystemKeyCaptureActive(), "fixture must begin with keyboard capture");
        input.captured = false; // The menu releases capture before invoking its action.
        input.updateKeyboardGrabState();
        input.toggle();
        check(input.m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF,
              "menu toggle must disable previously enabled system-key capture");
        check(!input.window.keyboardGrab, "menu action must not grab input before close");
        input.toggle();
        check(input.m_CaptureSystemKeysMode == StreamingPreferences::CSK_ALWAYS,
              "second menu toggle must enable capture");
        check(!input.window.keyboardGrab, "enabling policy must wait for menu capture restoration");
        input.captured = true;
        input.updateKeyboardGrabState();
        check(input.isSystemKeyCaptureActive(), "closing menu must apply newly enabled policy");
    } else if (std::strcmp(argv[1], "legacy") == 0) {
        for (int mode = 0; mode <= 2; ++mode) {
            for (bool fullscreen : { false, true }) {
                for (bool captured : { false, true }) {
                    input.m_CaptureSystemKeysMode =
                        static_cast<StreamingPreferences::CaptureSysKeysMode>(mode);
                    input.window.flags =
                        SDL_WINDOW_INPUT_FOCUS | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
                    input.captured = captured;
                    input.updateKeyboardGrabState();
                    const bool expected = captured && (mode == 2 || (mode == 1 && fullscreen));
                    check(input.window.keyboardGrab == expected, "legacy native grab changed");
                    check(input.isSystemKeyCaptureActive() == expected,
                          "legacy system-key routing changed");
                    input.window.flags &= ~SDL_WINDOW_INPUT_FOCUS;
                    check(!input.isSystemKeyCaptureActive(), "unfocused stream captures keys");
                }
            }
        }
        input.captured = true;
        input.m_CaptureSystemKeysMode = StreamingPreferences::CSK_FULLSCREEN;
        input.window.flags = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_FULLSCREEN_DESKTOP;
        input.updateKeyboardGrabState();
        check(input.isSystemKeyCaptureActive(),
              "legacy mode must still include SDL desktop fullscreen");
    } else if (std::strcmp(argv[1], "geometry") == 0) {
        input.window.flags |= SDL_WINDOW_BORDERLESS;
        check(input.session.isStreamingWindowBorderlessOnDisplay(),
              "covering borderless window not recognized");
        input.window.client.w -= 1;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "one-pixel gap must not qualify");
        input.window.client = input.window.display = QRect(-1920, -720, 1920, 1080);
        check(input.session.isStreamingWindowBorderlessOnDisplay(),
              "negative-origin monitor not recognized");
        input.window.client.x += 1;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "offset window must not qualify");
        input.window.client = input.window.display;
        input.window.flags &= ~SDL_WINDOW_BORDERLESS;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "decorated monitor-sized window qualifies");
        input.window.flags |= SDL_WINDOW_BORDERLESS;
        input.session.preferences.windowMode = StreamingPreferences::WM_WINDOWED;
        check(input.session.isStreamingWindowBorderlessOnDisplay(),
              "saved startup mode overrode live borderless geometry");
        input.session.preferences.windowMode = StreamingPreferences::WM_FULLSCREEN_DESKTOP;
        input.window.displayQueryOk = false;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "failed display query qualifies");
        check(warnings > 0, "failed display query must be logged");
        input.window.displayQueryOk = true;
        input.window.flags |= SDL_WINDOW_MINIMIZED;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(), "minimized window qualifies");
        input.window.flags &= ~SDL_WINDOW_MINIMIZED;
        QtWindow wayland;
        input.session.m_WaylandStreamWindow = &wayland;
        check(input.session.isStreamingWindowBorderlessOnDisplay(),
              "Qt logical geometry mixed with SDL pixels");
        wayland.bounds.h -= 1;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "partial Qt monitor coverage qualifies");
        wayland.bounds = wayland.display.bounds;
        wayland.style = 0;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(),
              "decorated Qt window qualifies");
        wayland.style = Qt::FramelessWindowHint;
        wayland.output = nullptr;
        check(!input.session.isStreamingWindowBorderlessOnDisplay(), "missing Qt screen qualifies");
    } else if (std::strcmp(argv[1], "borderless") == 0) {
        input.m_CaptureSystemKeysMode = static_cast<StreamingPreferences::CaptureSysKeysMode>(3);
        input.updateKeyboardGrabState();
        check(!input.isSystemKeyCaptureActive(), "new mode must not capture a normal window");
        input.window.flags |= SDL_WINDOW_BORDERLESS;
        input.updateKeyboardGrabState();
        check(input.isSystemKeyCaptureActive(),
              "new mode must capture a covering borderless window");
        input.window.client.w -= 1;
        input.updateKeyboardGrabState();
        check(!input.window.keyboardGrab && !input.isSystemKeyCaptureActive(),
              "resizing out of monitor coverage must release keyboard capture");
        input.window.flags = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_FULLSCREEN;
        input.updateKeyboardGrabState();
        check(input.isSystemKeyCaptureActive(),
              "fullscreen transition must restore keyboard capture");
        input.window.client = input.window.display;
        input.window.flags = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_BORDERLESS;
        input.handleMouseReleaseHold(true);
        check(!input.isCaptureActive() && !input.isSystemKeyCaptureActive(),
              "Right Alt did not release input");
        input.window.flags = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_FULLSCREEN;
        input.updateKeyboardGrabState();
        input.setCaptureActive(true);
        check(!input.window.keyboardGrab && !input.isCaptureActive(),
              "fullscreen recaptured during Right Alt hold");
        input.window.flags = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_BORDERLESS;
        input.updateKeyboardGrabState();
        input.handleMouseReleaseHold(false);
        check(input.isSystemKeyCaptureActive(),
              "Right Alt release did not apply current borderless policy");
        for (int tap = 0; tap < 8; ++tap) {
            input.handleMouseReleaseHold(true);
            input.handleMouseReleaseHold(false);
            check(input.captured, "repeated taps must not latch free mode");
        }
        input.handleMouseReleaseHold(true);
        input.window.flags &= ~SDL_WINDOW_INPUT_FOCUS;
        input.handleMouseReleaseHold(false);
        check(!input.isCaptureActive(), "Right Alt release recaptured an unfocused stream");
    } else if (std::strcmp(argv[1], "overlay") == 0) {
        for (bool captured : { true, false }) {
            for (bool doneBeforeKeyUp : { true, false }) {
                SdlInputHandler owner;
                owner.session.wireOverlayCallbacks();
                owner.setCaptureActive(captured);
                owner.handleMouseReleaseHold(true);
                check(owner.session.stats.armed && !owner.captured,
                      "hold must arm perf click-to-edit and release capture immediately");
                check(!owner.session.menu.editing, "hold must not arm menu editing");
                owner.session.m_WasCapturedBeforeMenu = captured;
                owner.session.stats.editCallback(true);
                check(!owner.m_MouseHoldReleased,
                      "editor must unconditionally take hold restore ownership from menu");
                if (doneBeforeKeyUp) {
                    owner.session.stats.editCallback(false);
                    check(!owner.captured, "Done during hold must defer recapture until key-up");
                }
                owner.handleMouseReleaseHold(false);
                check(!owner.session.stats.armed, "key-up must disarm future edit clicks");
                if (!doneBeforeKeyUp) {
                    check(!owner.captured, "key-up recaptured while editor owns pointer");
                    owner.session.stats.editCallback(false);
                }
                check(owner.captured == captured, "editor changed initially-free capture state");
            }
        }
        SdlInputHandler owner;
        owner.session.wireOverlayCallbacks();
        owner.session.menu.visible = true;
        owner.handleMouseReleaseHold(true);
        owner.handleMouseReleaseHold(false);
        check(owner.session.menu.visible && !owner.session.menu.editing,
              "hold must leave menu visible and its clicks as normal actions");
        owner.handleMouseReleaseHold(true);
        owner.window.flags &= ~SDL_WINDOW_INPUT_FOCUS;
        owner.notifyFocusLost();
        check(!owner.session.stats.armed && !owner.m_MouseHoldReleased,
              "focus loss must clear hold arming and deferred capture");
        owner.window.flags |= SDL_WINDOW_INPUT_FOCUS;
        owner.handleMouseReleaseHold(false);
        check(!owner.captured, "stale key-up after focus loss recaptured input");
        owner.session.menu.visible = false;
        owner.setCaptureActive(true);
        owner.handleMouseReleaseHold(true);
        owner.session.stats.editCallback(true);
        owner.window.flags &= ~SDL_WINDOW_INPUT_FOCUS;
        owner.notifyFocusLost();
        owner.session.stats.editCallback(false);
        check(!owner.captured,
              "Done on an unfocused editor must not recapture another app's mouse");
    } else {
        return 2;
    }
    return failures ? 1 : 0;
}
