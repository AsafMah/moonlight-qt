#pragma once

#include "SDL_compat.h"

class StreamUtils
{
public:
    static
    Uint32 getPlatformWindowFlags();

    static
    SDL_Window* createTestWindow();

    static
    void scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_FRect* rect, int viewportWidth, int viewportHeight);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_Rect* src, SDL_FRect* dst, int viewportWidth, int viewportHeight);

    static
    bool getNativeDesktopMode(int displayIndex, SDL_DisplayMode* mode, SDL_Rect* safeArea);

    static
    int getDisplayRefreshRate(SDL_Window* window);

    // The exact refresh rate (numerator / denominator Hz) of the display
    // showing the window, where the OS reports it (Windows). False if unknown.
    static bool getExactDisplayRefreshRate(SDL_Window* window, uint32_t* numerator,
                                           uint32_t* denominator);

    // The same for a native window (HWND on Windows)
    static bool getExactNativeWindowRefreshRate(void* nativeWindow, uint32_t* numerator,
                                                uint32_t* denominator);

    static
    bool hasFastAes();

    static
    int getDrmFdForWindow(SDL_Window* window, bool* needsClose);

    static
    int getDrmFd(bool preferRenderNode);

    static
    void enterAsyncLoggingMode();

    static
    void exitAsyncLoggingMode();
};
