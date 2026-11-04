#include <iostream>

using Uint32 = unsigned int;
constexpr Uint32 SDL_WINDOW_SHOWN = 1;
constexpr Uint32 SDL_WINDOW_HIDDEN = 2;
constexpr Uint32 SDL_WINDOW_MINIMIZED = 4;
constexpr Uint32 SDL_WINDOW_INPUT_FOCUS = 8;
constexpr int SDL_WINDOWEVENT_FOCUS_LOST = 1;
constexpr int SDL_WINDOWEVENT_FOCUS_GAINED = 2;

struct Window
{
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    bool active = false;
    bool isVisible() const { return (flags & SDL_WINDOW_SHOWN) != 0; }
    bool isActive() const { return active; }
};

Uint32 SDL_GetWindowFlags(Window* window)
{
    return window->flags;
}

struct QRect
{
    bool isValid() const { return true; }
    int x() const { return 0; }
    int y() const { return 0; }
    int width() const { return 1280; }
    int height() const { return 720; }
};

struct Button
{
    bool visible = false;
    void showButton(int, int, int, int) { visible = true; }
    void hideButton() { visible = false; }
};

struct Panel
{
    bool visible = false;
    bool closing = false;
    bool isMenuVisible() const { return visible; }
    bool isClosing() const { return closing; }
    void closeMenu()
    {
        visible = false;
        closing = true;
    }
};

struct Toast
{
    bool visible = true;
    void dismissImmediately() { visible = false; }
};

struct Stylus
{
    bool pending = true;
    void closePanel() { pending = false; }
};

struct Stats
{
    bool streamVisible = false;
    void syncWithStreamWindow(bool visible) { streamVisible = visible; }
};

struct Input
{
    bool focused = true;
    void notifyFocusLost() { focused = false; }
    void notifyFocusGained() { focused = true; }
    void reloadShortcuts() {}
};

class Session
{
public:
    Window window;
    Window* m_Window = &window;
    Window* m_WaylandStreamWindow = nullptr;
    Button button;
    Button* m_MenuButton = &button;
    Panel panel;
    Panel* m_MenuPanel = &panel;
    Toast toast;
    Toast* m_Toast = &toast;
    Stylus stylus;
    Stylus* m_StylusReplayTest = &stylus;
    Stats stats;
    Stats* m_StatsOverlay = &stats;
    Input input;
    Input* m_InputHandler = &input;
    struct
    {
        bool buttonEnabled = true;
    } m_OverlayMenuOptions;
    bool m_WaylandOverlayShortcutPressed = true;
    bool m_StreamInputFocused = true;
    bool audioFocused = true;
    void updateAudioMuteState() { audioFocused = m_StreamInputFocused; }
    QRect qtOverlayParentGeometry() const { return {}; }
    bool isStreamingWindowVisible() const;
    bool isStreamingWindowFocused() const;
    bool isStreamingWindowMinimized() const;
    void syncQtOverlayWindowsWithSdlWindowState();
    void focusEvent(int event);
};

// METHODS

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

int main()
{
    bool ok = true;
    Session session;
    session.syncQtOverlayWindowsWithSdlWindowState();
    ok &= require(session.button.visible, "Focused stream must show the enabled button");
    session.panel.visible = true;
    session.window.flags &= ~SDL_WINDOW_INPUT_FOCUS;
    session.focusEvent(SDL_WINDOWEVENT_FOCUS_LOST);
    ok &= require(!session.button.visible, "Focus loss must hide the floating button");
    ok &= require(!session.panel.visible && !session.toast.visible && !session.stylus.pending,
                  "Focus loss must close the menu, toast and pending stylus panel");
    ok &= require(!session.input.focused && !session.audioFocused &&
                      !session.m_WaylandOverlayShortcutPressed,
                  "Focus loss must preserve input, audio and shortcut cleanup");
    ok &= require(session.stats.streamVisible,
                  "Focus must not replace the stats controller's stream visibility policy");
    session.syncQtOverlayWindowsWithSdlWindowState();
    ok &= require(!session.button.visible, "A menu-close or geometry sync must not reshow it");
    session.window.flags |= SDL_WINDOW_INPUT_FOCUS;
    session.focusEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
    ok &= require(!session.button.visible, "A closing menu must still suppress the button");
    session.panel.closing = false;
    session.focusEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
    ok &= require(session.button.visible && session.input.focused && session.audioFocused,
                  "Refocus must restore the enabled button and input/audio state");
    session.m_OverlayMenuOptions.buttonEnabled = false;
    session.focusEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
    ok &= require(!session.button.visible, "Refocus must not enable a disabled button");
    session.m_OverlayMenuOptions.buttonEnabled = true;
    session.window.flags = SDL_WINDOW_SHOWN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_INPUT_FOCUS;
    session.focusEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
    ok &= require(!session.button.visible && !session.stats.streamVisible,
                  "Refocus must not show overlays over a minimized stream");
    session.window.flags = SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    Window wayland;
    session.m_WaylandStreamWindow = &wayland;
    session.syncQtOverlayWindowsWithSdlWindowState();
    ok &= require(!session.button.visible, "Wayland focus must come from the Qt window, not SDL");
    wayland.active = true;
    session.syncQtOverlayWindowsWithSdlWindowState();
    ok &=
        require(session.button.visible, "An active visible Wayland stream must restore the button");
    session.m_WaylandStreamWindow = nullptr;
    session.m_Window = nullptr;
    session.syncQtOverlayWindowsWithSdlWindowState();
    ok &= require(!session.button.visible, "A missing stream window must hide the button");
    return ok ? 0 : 1;
}
