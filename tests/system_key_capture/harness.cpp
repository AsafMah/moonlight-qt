#include <cstdio>
#include <cstring>
#include <initializer_list>

#define SDL_VERSION_ATLEAST(a, b, c) 1
using Uint32 = unsigned int;
constexpr int SDL_TRUE = 1, SDL_FALSE = 0;
constexpr Uint32 SDL_WINDOW_INPUT_FOCUS = 1, SDL_WINDOW_FULLSCREEN = 2;
constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
void SDL_LogInfo(int, const char*) {}

struct StreamingPreferences
{
    // CAPTURE_ENUM
};

struct SDL_Window
{
    Uint32 flags = SDL_WINDOW_INPUT_FOCUS;
    bool keyboardGrab = false;
};
Uint32 SDL_GetWindowFlags(SDL_Window* window)
{
    return window->flags;
}
void SDL_SetWindowKeyboardGrab(SDL_Window* window, int grab)
{
    window->keyboardGrab = grab;
}
bool isStreamWindowFullscreen(SDL_Window* window)
{
    return window && (window->flags & SDL_WINDOW_FULLSCREEN);
}

class SdlInputHandler
{
public:
    SDL_Window window;
    SDL_Window* m_Window = &window;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode =
        StreamingPreferences::CSK_ALWAYS;
    bool captured = true;
    bool m_KeyboardCaptureActive = false;
    bool isCaptureActive() { return captured; }
    bool isSystemKeyCaptureActive();
    bool isSystemKeyCaptureEnabledForWindow();
    void updateKeyboardGrabState();
    void toggle();
};

// INPUT_METHODS

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
    } else {
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
    }
    return failures ? 1 : 0;
}
