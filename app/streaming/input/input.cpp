#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/session.h"
#include "settings/mappingmanager.h"
#include "path.h"
#include "utils.h"

#ifdef HAVE_WINDOWS_RAW_TOUCHPAD
#include "wintouchpad.h"
#endif

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>
#ifdef Q_OS_MACOS
#include <QImage>
#endif

// Include SDL_syswm.h after Qt headers to avoid X11 macro conflicts on Linux
#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#include <Windows.h>
#include <Imm.h>
#pragma comment(lib, "imm32.lib")
#endif

namespace {

// 主机推来的标准光标默认换成本机的系统光标。设 MOONLIGHT_NATIVE_CURSOR=0 可以不
// 重编就退回旧行为，用来确认某个形状是不是被认错了。
bool nativeCursorSubstitutionEnabled()
{
    static const bool enabled =
        qEnvironmentVariable("MOONLIGHT_NATIVE_CURSOR") != QStringLiteral("0");
    return enabled;
}

bool isStreamWindowFullscreen(SDL_Window* window)
{
    if (Session* session = Session::get()) {
        return session->isStreamingWindowFullscreen();
    }
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

bool toSdlSystemCursor(NativeCursorShape shape, SDL_SystemCursor& systemCursor)
{
    switch (shape) {
    case NativeCursorShape::Arrow:
        systemCursor = SDL_SYSTEM_CURSOR_ARROW;
        return true;
    case NativeCursorShape::AppStarting:
        // SDL 的 Cocoa 后端把它映射到 HIServices 的 busybutclickable —— 正是 macOS
        // 自己那只"箭头 + 转圈"。不会转（SDL 不支持动画光标），但风格是对的。
        systemCursor = SDL_SYSTEM_CURSOR_WAITARROW;
        return true;
    case NativeCursorShape::Wait:
        // SDL 的 Cocoa 后端把 WAIT 和 WAITARROW 映射到同一只 busybutclickable，所以
        // 主机只有一个圈时，本机显示的是"箭头 + 转圈"。macOS 没有纯转圈的公开光标
        // （沙滩球由 WindowServer 画，设不了），这已经是最接近的原生表达。
        systemCursor = SDL_SYSTEM_CURSOR_WAIT;
        return true;
    case NativeCursorShape::IBeam:
        systemCursor = SDL_SYSTEM_CURSOR_IBEAM;
        return true;
    case NativeCursorShape::Hand:
        systemCursor = SDL_SYSTEM_CURSOR_HAND;
        return true;
    case NativeCursorShape::SizeWE:
        systemCursor = SDL_SYSTEM_CURSOR_SIZEWE;
        return true;
    case NativeCursorShape::SizeNS:
        systemCursor = SDL_SYSTEM_CURSOR_SIZENS;
        return true;
    case NativeCursorShape::SizeNWSE:
        systemCursor = SDL_SYSTEM_CURSOR_SIZENWSE;
        return true;
    case NativeCursorShape::SizeNESW:
        systemCursor = SDL_SYSTEM_CURSOR_SIZENESW;
        return true;
    case NativeCursorShape::Unknown:
        break;
    }
    return false;
}

} // namespace

SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight,
                                 bool enablePhysicalDualSenseHaptics)
    : m_MultiController(prefs.multiController), m_GamepadMouse(prefs.gamepadMouse),
      m_EnableDualSenseHaptics(enablePhysicalDualSenseHaptics),
      m_SwapMouseButtons(prefs.swapMouseButtons), m_SwapWinAltKeys(prefs.swapWinAltKeys),
      m_ReverseScrollDirection(prefs.reverseScrollDirection),
      m_SwapFaceButtons(prefs.swapFaceButtons), m_GamepadQuitCombo(prefs.gamepadQuitCombo),
      m_GamepadQuitEnabled(qgetenv("NO_GAMEPAD_QUIT") != "1"),
      m_GamepadDeadzone(qBound(0, prefs.gamepadDeadzone, 30)), m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false), m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false), m_FakeMouseCaptureActive(false),
      m_KeyboardCaptureActive(false), m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_MouseCursorCapturedVisibilityState(prefs.showLocalCursor ? SDL_ENABLE : SDL_DISABLE),
      m_LocalCursorMode(prefs.absoluteMouseMode && prefs.showLocalCursor ? LI_CURSOR_MODE_LOCAL
                                                                         : LI_CURSOR_MODE_VIDEO),
      m_RemoteCursorVisible(true), m_RemoteCursor(nullptr),
      m_LastCursorClass(NativeCursorShape::Unknown), m_HasLastCursorShape(false),
      m_RemoteCursorScale(1.0), m_RemoteCursorHideTimer(0), m_LongPressTimer(0),
      m_StreamWidth(streamWidth), m_StreamHeight(streamHeight),
      m_AbsoluteMouseMode(prefs.absoluteMouseMode), m_AbsoluteTouchMode(prefs.absoluteTouchMode),
      m_DisabledTouchFeedback(false),
#ifdef HAVE_WINDOWS_PEN_INPUT
      m_WindowsPenWindow(nullptr), m_WindowsPenSubclassContext(nullptr), m_WindowsPenPointerId(0),
      m_WindowsPenFallbackPointerId(UINT32_MAX), m_WindowsPenSuppressedPointerId(UINT32_MAX),
      m_WindowsPenSubclassInstalled(false), m_WindowsPenPointerTracked(false),
      m_WindowsPenCancelPending(false), m_WindowsPenLastSentStateKey(0),
      m_WindowsPenLastSentStateValid(false), m_WindowsPenHistoryStats{},
#endif
      m_NativeTouchpadEnabled(SDL_GetHintBoolean(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, SDL_FALSE) ==
                              SDL_TRUE),
      m_TouchpadFlushEventQueued(false), m_NativeTouchpadTransport(NTT_UNKNOWN),
      m_PendingTouchpadId(0), m_PendingTouchpadTimestamp(0), m_PendingTouchpadContactCount(0),
      m_ActiveTouchpadId(0), m_LastTouchpadScrollTimestamp(0),
#ifdef HAVE_MACOS_NATIVE_TOUCHPAD
      m_MacTouchpadSuppressedMouseButtons(0), m_MacTouchpadPendingTapButtons(0),
      m_MacTouchpadLastTapTimestamp(0), m_MacTouchpadGestureStartTimestamp(0),
      m_MacTouchpadGesturePrimaryFinger(0), m_MacTouchpadGestureStartX(0),
      m_MacTouchpadGestureStartY(0), m_MacTouchpadGestureMaxContacts(0),
      m_MacTouchpadGestureMoved(false), m_MacTouchpadGestureHadPhysicalButton(false),
      m_MacTouchpadButtonDown(false),
#endif
#ifdef HAVE_WINDOWS_RAW_TOUCHPAD
      m_ActiveWindowsTouchpadDevice(0), m_LastWindowsTouchpadFrameTicks(0),
      m_SuppressedWindowsTouchpadMouseButtons(0), m_WindowsTouchpadButtonDown(false),
      m_WindowsTouchpadButtonUsesMouseFallback(false), m_WindowsTouchpadWidthMm(0),
      m_WindowsTouchpadHeightMm(0),
#endif
      m_LeftButtonReleaseTimer(0), m_RightButtonReleaseTimer(0), m_DragTimer(0), m_DragButton(0),
      m_NumFingersDown(0)
{
    // System keys are always captured when running without a DE
    if (!WMUtils::isRunningDesktopEnvironment()) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }

    // SDL3 breaks our auto-capture-on-leave logic because the mouse focus has already
    // been lost by the time we attempt to call SDL_CaptureMouse(). Fortunately, SDL3's
    // own auto-capture logic seems to be stable now (unlike SDL2), so we can rely on
    // that instead of our own hack when running on sdl2-compat.
    // https://github.com/libsdl-org/SDL/commit/e54001b02809dcebbb822bd0297919c8c76976a1
    SDL_version ver;
    SDL_GetVersion(&ver);
    m_NeedsManualCaptureOnLeave = !(ver.major == 2 && ver.minor >= 30 && ver.patch >= 50) && !SDL_GetHint("SDL3_VERSION");
    if (m_NeedsManualCaptureOnLeave) {
        // Disable the buggy auto-capture on earlier SDL2 builds
        SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
    }

    // Allow gamepad input when the app doesn't have focus if requested
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, prefs.backgroundGamepad ? "1" : "0");

#if !SDL_VERSION_ATLEAST(2, 0, 15)
    // For older versions of SDL (2.0.14 and earlier), use SDL_HINT_GRAB_KEYBOARD
    SDL_SetHintWithPriority(SDL_HINT_GRAB_KEYBOARD,
                            m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF ? "1" : "0",
                            SDL_HINT_OVERRIDE);
#endif

    // Opt-out of SDL's built-in Alt+Tab handling while keyboard grab is enabled
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");

#ifdef Q_OS_WIN32
    // Keep Alt+F4 in the keyboard input path for the entire streaming session,
    // including when the stream is running in a window.
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
#endif

    // Allow clicks to pass through to us when focusing the window. If we're in
    // absolute mouse mode, this will avoid the user having to click twice to
    // trigger a click on the host if the Moonlight window is not focused. In
    // relative mode, the click event will trigger the mouse to be recaptured.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // Enabling extended input reports allows rumble to function on Bluetooth PS4/PS5
    // controllers, but breaks DirectInput applications. We will enable it because
    // it's likely that working rumble is what the user is expecting. If they don't
    // want this behavior, they can override it with the environment variable.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");

    // Keyboard shortcuts. Actions that need a desktop environment get no
    // handler without one, so their keys are passed through to the host.
    const bool hasDesktopEnvironment = WMUtils::isRunningDesktopEnvironment();
    const struct
    {
        const char* actionId;
        KeyCombo combo;
        bool enabled;
    } shortcutHandlers[] = {
        { ShortcutActions::Quit, KeyComboQuit, true },
        { ShortcutActions::UngrabInput, KeyComboUngrabInput, hasDesktopEnvironment },
        { ShortcutActions::ToggleFullScreen, KeyComboToggleFullScreen, hasDesktopEnvironment },
        { ShortcutActions::ToggleStatsOverlay, KeyComboToggleStatsOverlay, true },
        { ShortcutActions::ToggleMouseMode, KeyComboToggleMouseMode, true },
        { ShortcutActions::ToggleCursorHide, KeyComboToggleCursorHide, true },
        { ShortcutActions::Minimize, KeyComboToggleMinimize, hasDesktopEnvironment },
        { ShortcutActions::PasteText, KeyComboPasteText, true },
        { ShortcutActions::TogglePointerRegionLock, KeyComboTogglePointerRegionLock, true },
        { ShortcutActions::QuitAndExit, KeyComboQuitAndExit, true },
        { ShortcutActions::ToggleKeyboardGrab, KeyComboToggleKeyboardGrab, hasDesktopEnvironment },
    };
    for (const auto& shortcut : shortcutHandlers) {
        if (shortcut.enabled) {
            const KeyCombo combo = shortcut.combo;
            m_ShortcutDispatcher.setHandler(QString::fromLatin1(shortcut.actionId),
                                            [this, combo] { performSpecialKeyCombo(combo); });
        }
    }
    if (hasDesktopEnvironment) {
        m_MouseReleaseClock.start();
        m_ShortcutDispatcher.setHoldHandler(
            QString::fromLatin1(ShortcutActions::RightAltMouseRelease),
            [this](bool held) { handleMouseReleaseHold(held); });
    }
    reloadShortcuts();

    m_OldIgnoreDevices = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES);
    m_OldIgnoreDevicesExcept = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);

    QString streamIgnoreDevices = qgetenv("STREAM_GAMECONTROLLER_IGNORE_DEVICES");
    QString streamIgnoreDevicesExcept = qgetenv("STREAM_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT");

    if (!streamIgnoreDevices.isEmpty() && !streamIgnoreDevices.endsWith(',')) {
        streamIgnoreDevices += ',';
    }
    streamIgnoreDevices += m_OldIgnoreDevices;

    // STREAM_IGNORE_DEVICE_GUIDS allows to specify additional devices to be ignored when starting
    // the stream in case the scope of STREAM_GAMECONTROLLER_IGNORE_DEVICES is too broad. One such
    // case is "Steam Virtual Gamepad" where everything is under the same VID/PID, but different GUIDs.
    // Multiple GUIDs can be provided, but need to be separated by commas:
    //
    //     <GUID>,<GUID>,<GUID>,...
    //
    QString streamIgnoreDeviceGuids = qgetenv("STREAM_IGNORE_DEVICE_GUIDS");
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    m_IgnoreDeviceGuids = streamIgnoreDeviceGuids.split(',', Qt::SkipEmptyParts);
#else
    m_IgnoreDeviceGuids = streamIgnoreDeviceGuids.split(',', QString::SkipEmptyParts);
#endif

    // For SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, we use the union of SDL_GAMECONTROLLER_IGNORE_DEVICES
    // and STREAM_GAMECONTROLLER_IGNORE_DEVICES while streaming. STREAM_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT
    // overrides SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT while streaming.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, streamIgnoreDevices.toUtf8());
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, streamIgnoreDevicesExcept.toUtf8());

    // We must initialize joystick explicitly before gamecontroller in order
    // to ensure we receive gamecontroller attach events for gamepads where
    // SDL doesn't have a built-in mapping. By starting joystick first, we
    // can allow mapping manager to update the mappings before GC attach
    // events are generated.
    SDL_assert(!SDL_WasInit(SDL_INIT_JOYSTICK));
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_JOYSTICK) failed: %s",
                     SDL_GetError());
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Flush gamepad arrival and departure events which may be queued before
    // starting the gamecontroller subsystem again. This prevents us from
    // receiving duplicate arrival and departure events for the same gamepad.
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);
    SDL_FlushEvent(SDL_CONTROLLERDEVICEREMOVED);

    // We need to reinit this each time, since you only get
    // an initial set of gamepad arrival events once per init.
    SDL_assert(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
    }

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_assert(!SDL_WasInit(SDL_INIT_HAPTIC));
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_HAPTIC) failed: %s",
                     SDL_GetError());
    }
#endif

    // Initialize the gamepad mask with currently attached gamepads to avoid
    // causing gamepads to unexpectedly disappear and reappear on the host
    // during stream startup as we detect currently attached gamepads one at a time.
    m_GamepadMask = getAttachedGamepadMask();

    SDL_zero(m_GamepadState);
    SDL_zero(m_LastTouchDownEvent);
    SDL_zero(m_LastTouchUpEvent);
    SDL_zero(m_TouchDownEvent);
}

SdlInputHandler::~SdlInputHandler()
{
#ifdef HAVE_WINDOWS_PEN_INPUT
    shutdownWindowsPenInput();
#endif
#ifdef Q_OS_WIN32
    // Restore SDL's normal Alt+F4 handling outside the streaming session.
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "0");
#endif

#ifdef HAVE_WINDOWS_RAW_TOUCHPAD
    m_WindowsTouchpadInput.reset();
#endif
    cancelNativeTouchpadContacts();
    resetRemoteCursor();
    stopAllRumble();

    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].mouseEmulationTimer != 0) {
            Session::get()->notifyMouseEmulationMode(false);
            SDL_RemoveTimer(m_GamepadState[i].mouseEmulationTimer);
            // 析构是模拟鼠标键释放的第四条路径:会话结束时按住的键同样要
            // 还给主机,否则主机侧鼠标键卡死
            releaseMouseEmulationButtons(&m_GamepadState[i]);
        }
#if !SDL_VERSION_ATLEAST(2, 0, 9)
        if (m_GamepadState[i].haptic != nullptr) {
            SDL_HapticClose(m_GamepadState[i].haptic);
        }
#endif
        if (m_GamepadState[i].controller != nullptr) {
            SDL_GameControllerClose(m_GamepadState[i].controller);
        }
    }

    SDL_RemoveTimer(m_LongPressTimer);
    SDL_RemoveTimer(m_LeftButtonReleaseTimer);
    SDL_RemoveTimer(m_RightButtonReleaseTimer);
    SDL_RemoveTimer(m_DragTimer);
    SDL_RemoveTimer(m_RemoteCursorHideTimer);
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    SDL_assert(!SDL_WasInit(SDL_INIT_HAPTIC));
#endif

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_assert(!SDL_WasInit(SDL_INIT_GAMECONTROLLER));

    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    SDL_assert(!SDL_WasInit(SDL_INIT_JOYSTICK));

    // Return background event handling to off
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");

    // Restore the ignored devices
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, m_OldIgnoreDevices.toUtf8());
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, m_OldIgnoreDevicesExcept.toUtf8());

#ifdef STEAM_LINK
    // Hide SDL's cursor on Steam Link after quitting the stream.
    // FIXME: We should also do this for other situations where SDL
    // and Qt will draw their own mouse cursors like KMSDRM or RPi
    // video backends.
    SDL_ShowCursor(SDL_DISABLE);
#endif
}

void SdlInputHandler::setWindow(SDL_Window *window)
{
    m_Window = window;
    m_DisabledTouchFeedback = false;

#ifdef HAVE_WINDOWS_PEN_INPUT
    initializeWindowsPenInput();
#endif

#ifdef HAVE_WINDOWS_RAW_TOUCHPAD
    if (m_NativeTouchpadEnabled && !m_WindowsTouchpadInput) {
        auto windowsTouchpadInput = std::make_unique<WindowsTouchpadInput>(this);
        if (windowsTouchpadInput->initialize(window)) {
            m_WindowsTouchpadInput = std::move(windowsTouchpadInput);
        }
    }
#endif

#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_Window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        ImmAssociateContext(info.info.win.window, NULL);
    }
#endif
}

void SdlInputHandler::setStreamDimensions(int streamWidth, int streamHeight)
{
    m_StreamWidth = streamWidth;
    m_StreamHeight = streamHeight;
}

#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
void SdlInputHandler::setWaylandWindowCoordinateMetrics(
    WaylandWindowMetrics::CoordinateMetrics metrics)
{
    if (metrics.isValid()) {
        m_WaylandCoordinateMetrics = metrics;
    } else {
        m_WaylandCoordinateMetrics.reset();
    }
}
#endif

void SdlInputHandler::getWindowCoordinateSize(int* width, int* height) const
{
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    if (m_WaylandCoordinateMetrics.has_value()) {
        *width = m_WaylandCoordinateMetrics->window.width;
        *height = m_WaylandCoordinateMetrics->window.height;
        return;
    }
#endif
    SDL_GetWindowSize(m_Window, width, height);
}

int SdlInputHandler::getLocalCursorMode() const
{
    return m_LocalCursorMode.load(std::memory_order_relaxed);
}

int SdlInputHandler::getCapturedCursorVisibilityState() const
{
    if (m_MouseCursorCapturedVisibilityState == SDL_DISABLE) {
        return SDL_DISABLE;
    }

    if (getLocalCursorMode() == LI_CURSOR_MODE_LOCAL &&
        (LiGetHostFeatureFlags() & LI_FF_CURSOR_SHAPE) != 0) {
        return m_RemoteCursorVisible ? SDL_ENABLE : SDL_DISABLE;
    }

    return SDL_ENABLE;
}

void SdlInputHandler::applyCapturedCursorState()
{
    if (getLocalCursorMode() == LI_CURSOR_MODE_LOCAL &&
        m_RemoteCursor != nullptr) {
        SDL_SetCursor(m_RemoteCursor);
    }
    SDL_ShowCursor(getCapturedCursorVisibilityState());
}

Uint32 SdlInputHandler::remoteCursorHideTimerCallback(Uint32 interval, void*)
{
    // 定时器跑在 SDL 的定时器线程上，macOS 要求光标接口只在主线程调，绕回主循环去做
    if (!Session::queueCursorVisibilityFlush()) {
        // 事件队列满了。这里必须重试而不是就此收工：定时器一停，
        // m_RemoteCursorHideTimer 就永远卡在一个已过期的 ID 上，
        // updateRemoteCursorVisibility() 之后每次隐藏请求都会被那道
        // "已经在等了" 的短路挡掉，直到主机下次说要显示才解开。
        return interval;
    }
    return 0;
}

void SdlInputHandler::cancelPendingRemoteCursorHide()
{
    if (m_RemoteCursorHideTimer != 0) {
        SDL_RemoveTimer(m_RemoteCursorHideTimer);
        m_RemoteCursorHideTimer = 0;
    }
}

void SdlInputHandler::updateRemoteCursorVisibility(bool visible)
{
    if (visible) {
        // 显示立即生效，同时把还没到期的隐藏作废
        cancelPendingRemoteCursorHide();
        m_RemoteCursorVisible = true;
        return;
    }

    // 已经藏了，或者已经在等这次隐藏坐实
    if (!m_RemoteCursorVisible || m_RemoteCursorHideTimer != 0) {
        return;
    }

    m_RemoteCursorHideTimer = SDL_AddTimer(REMOTE_CURSOR_HIDE_DEBOUNCE_MS,
                                           remoteCursorHideTimerCallback,
                                           this);
    if (m_RemoteCursorHideTimer == 0) {
        // 定时器起不来就立即生效。宁可闪，也不能把主机要求藏起来的光标留在屏上。
        m_RemoteCursorVisible = false;
    }
}

void SdlInputHandler::flushPendingRemoteCursorHide()
{
    if (m_RemoteCursorHideTimer == 0) {
        // 事件还在队列里排着的时候主机又说要显示，这次隐藏已经作废了
        return;
    }

    m_RemoteCursorHideTimer = 0;
    m_RemoteCursorVisible = false;

    if (isCaptureActive()) {
        applyCapturedCursorState();
    }
}

void SdlInputHandler::resetRemoteCursor()
{
    cancelPendingRemoteCursorHide();

    if (m_RemoteCursor == nullptr) {
        return;
    }

    if (SDL_GetCursor() == m_RemoteCursor) {
        SDL_SetCursor(SDL_GetDefaultCursor());
    }
    SDL_FreeCursor(m_RemoteCursor);
    m_RemoteCursor = nullptr;
}

void SdlInputHandler::installRemoteCursor(SDL_Cursor* cursor)
{
    SDL_Cursor* old = m_RemoteCursor;
    m_RemoteCursor = cursor;

    // 先把新光标装上，再放掉旧的。反过来的话释放的正是当前光标，SDL 会先跳回默认
    // 光标，于是每次换形状都能看见一帧默认箭头 —— 主机在两个形状之间来回推时（鼠标
    // 压在文本框边界上就会）这一帧就成了肉眼可见的闪烁。
    if (old != nullptr && SDL_GetCursor() == old) {
        SDL_SetCursor(cursor);
    }
    SDL_FreeCursor(old);
}

bool SdlInputHandler::tryUseNativeRemoteCursor(const RemoteCursorUpdate& update)
{
    const CursorShapeMetrics metrics =
        nativeCursorSubstitutionEnabled()
            ? measureCursorShape(update.width, update.height,
                                 update.hotspotX, update.hotspotY, update.bgra)
            : CursorShapeMetrics();
    const NativeCursorShape shape = classifyCursorShape(metrics);

    if (shape != m_LastCursorClass) {
        // 认不出来的时候把度量全量打出来，好照着实测数字调阈值——否则只知道"没认
        // 出来"，看不出被哪条判据挡掉了。走 verbose，默认日志级别下不出现；只在结果
        // 变化时打，所以游戏里逐帧变的自绘光标也不会刷屏。
        if (shape == NativeCursorShape::Unknown && metrics.valid) {
            SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION,
                           "Unclassified remote cursor %ux%u: "
                           "box=%dx%d fill=%.3f hotspot=(%.2f,%.2f) "
                           "symV=%.3f symH=%.3f symRot180=%.3f corr=%.3f "
                           "centerFill=%.3f firstRow=%.3f widestRow=%.3f "
                           "topWidth=%.3f violations=%d",
                           update.width, update.height,
                           metrics.boxWidth, metrics.boxHeight, metrics.fill,
                           metrics.hotspotX, metrics.hotspotY,
                           metrics.symV, metrics.symH, metrics.symRot180,
                           metrics.diagonalCorrelation,
                           metrics.centerFill,
                           metrics.firstRowRatio, metrics.widestRowPosition,
                           metrics.topWidthRatio, metrics.topMonotoneViolations);
        }

        m_LastCursorClass = shape;
    }
    else if (shape != NativeCursorShape::Unknown && m_RemoteCursor != nullptr) {
        // 同一种系统光标已经装着了。主机会反复推同一个形状，这里挡掉重建。
        return true;
    }

    SDL_SystemCursor systemCursor;
    if (!toSdlSystemCursor(shape, systemCursor)) {
        return false;
    }

    SDL_Cursor* cursor = SDL_CreateSystemCursor(systemCursor);
    if (cursor == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create system cursor %d: %s",
                    (int)systemCursor, SDL_GetError());
        // 别让上面那条"同一形状就短路"的分支把这次失败当成已经装好了
        m_LastCursorClass = NativeCursorShape::Unknown;
        return false;
    }

    // 热点由系统光标自带，主机推来的热点在这条路径上忽略
    installRemoteCursor(cursor);

    // 系统光标的尺寸由系统自己管，不受窗口 backing 比例影响，所以不必留着形状
    // 等换屏时重建 —— 顺带让 refreshRemoteCursorScale() 直接短路掉。
    m_HasLastCursorShape = false;
    m_RemoteCursorScale = getRemoteCursorScale();
    return true;
}

void SdlInputHandler::synchronizeLocalCursorMode()
{
    const int cursorMode = m_AbsoluteMouseMode &&
                           m_MouseCursorCapturedVisibilityState == SDL_ENABLE ?
                               LI_CURSOR_MODE_LOCAL : LI_CURSOR_MODE_VIDEO;
    m_LocalCursorMode.store(cursorMode, std::memory_order_relaxed);
    const int result = LiSetCursorMode(cursorMode);
    if (result != LI_CURSOR_MODE_OK &&
        result != LI_CURSOR_MODE_ERR_UNSUPPORTED &&
        result != LI_CURSOR_MODE_ERR_NOT_CONNECTED) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to synchronize local cursor mode: %d",
                    result);
    }

    if (cursorMode == LI_CURSOR_MODE_VIDEO) {
        resetRemoteCursor();
    }
    else {
        // The host will immediately send the authoritative state. Keep the
        // default cursor visible until that update arrives.
        cancelPendingRemoteCursorHide();
        m_RemoteCursorVisible = true;
    }
}

qreal SdlInputHandler::getRemoteCursorScale() const
{
#ifdef Q_OS_MACOS
    // SDL 的 Cocoa 后端把 surface 的像素宽高当成 NSImage 的逻辑 point 尺寸，所以
    // Retina 屏上要先按 backing 比例把光标缩回去。比例从窗口的 point / pixel 尺寸算，
    // 而不是问屏幕 —— 窗口横跨两块屏时它反映的是实际渲染用的那个 backing store。
    if (m_Window == nullptr) {
        return 1.0;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);

    if (windowWidth <= 0 || windowHeight <= 0 ||
        pixelWidth < windowWidth || pixelHeight < windowHeight) {
        return 1.0;
    }

    return static_cast<qreal>(pixelWidth) / windowWidth;
#else
    return 1.0;
#endif
}

void SdlInputHandler::refreshRemoteCursorScale()
{
#ifdef Q_OS_MACOS
    // 缩放比例是创建光标那一刻算的，而 updateRemoteCursor() 只在主机推来新形状时才会
    // 被调用。窗口从 Retina 屏拖到 1x 屏（或反过来）时，光标会一直停在旧比例上 ——
    // 大一倍或小一倍 —— 直到主机下一次换形状。这里用缓存的那份形状重建一次。
    if (!m_HasLastCursorShape) {
        return;
    }

    const qreal scale = getRemoteCursorScale();
    if (qFuzzyCompare(scale, m_RemoteCursorScale)) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Rebuilding remote cursor for backing scale %f -> %f",
                m_RemoteCursorScale, scale);
    updateRemoteCursor(m_LastCursorShape);
#endif
}

void SdlInputHandler::updateRemoteCursor(const RemoteCursorUpdate& update)
{
    updateRemoteCursorVisibility(update.visible);

    if (update.hasShape) {
        const qint64 expectedSize =
            static_cast<qint64>(update.width) * update.height * 4;
        if (update.width == 0 || update.height == 0 ||
            update.hotspotX < 0 || update.hotspotX >= update.width ||
            update.hotspotY < 0 || update.hotspotY >= update.height ||
            update.bgra.size() != expectedSize) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Ignoring invalid remote cursor shape %u",
                        update.shapeId);
        }
        else if (!tryUseNativeRemoteCursor(update)) {
            int cursorWidth = update.width;
            int cursorHeight = update.height;
            int hotspotX = update.hotspotX;
            int hotspotY = update.hotspotY;
            int cursorPitch = update.width * 4;
            const char* cursorPixels = update.bgra.constData();

#ifdef Q_OS_MACOS
            QImage scaledCursor;
            const qreal scale = getRemoteCursorScale();
            if (scale > 1.0) {
                cursorWidth = qMax(1, qRound(update.width / scale));
                cursorHeight = qMax(1, qRound(update.height / scale));
                hotspotX = qBound(0, qRound(update.hotspotX / scale), cursorWidth - 1);
                hotspotY = qBound(0, qRound(update.hotspotY / scale), cursorHeight - 1);

                if (cursorWidth != update.width || cursorHeight != update.height) {
                    const QImage source(
                        reinterpret_cast<const uchar*>(update.bgra.constData()),
                        update.width,
                        update.height,
                        update.width * 4,
                        QImage::Format_ARGB32);
                    // 先转预乘再缩。直接对直通 alpha 做 SmoothTransformation 的话，
                    // Qt 会把全透明像素里的黑色一起插值进来，缩完边缘一圈发暗。
                    scaledCursor = source
                                       .convertToFormat(QImage::Format_ARGB32_Premultiplied)
                                       .scaled(cursorWidth,
                                               cursorHeight,
                                               Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation)
                                       .convertToFormat(QImage::Format_ARGB32);
                    cursorPixels = reinterpret_cast<const char*>(scaledCursor.constBits());
                    cursorPitch = scaledCursor.bytesPerLine();
                }
            }
#endif

            SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
                const_cast<char*>(cursorPixels),
                cursorWidth,
                cursorHeight,
                32,
                cursorPitch,
                SDL_PIXELFORMAT_BGRA32);
            if (surface == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Failed to create remote cursor surface: %s",
                            SDL_GetError());
            }
            else {
                SDL_Cursor* cursor =
                    SDL_CreateColorCursor(surface, hotspotX, hotspotY);
                SDL_FreeSurface(surface);
                if (cursor == nullptr) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Failed to create remote cursor: %s",
                                SDL_GetError());
                }
                else {
                    installRemoteCursor(cursor);

                    // 记下这一份形状和它用的缩放比例。换显示器时靠它重建 ——
                    // 主机不会因为我们换了屏就重推一次形状。
                    m_LastCursorShape = update;
                    m_HasLastCursorShape = true;
                    m_RemoteCursorScale = getRemoteCursorScale();
                }
            }
        }
    }

    if (isCaptureActive()) {
        applyCapturedCursorState();
    }
}

void SdlInputHandler::notifyFocusLost()
{
    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Input focus lost: windowFlags=0x%08x keysDown=%d capture=%d",
                (unsigned int)windowFlags,
                (int)m_KeysDown.count(),
                isCaptureActive() ? 1 : 0);

    cancelNativeTouchpadContacts();

#ifdef HAVE_WINDOWS_PEN_INPUT
    cancelWindowsPenInput(true);
#endif

    // Release mouse cursor when another window is activated (e.g. by using ALT+TAB).
    // This lets user to interact with our window's title bar and with the buttons in it.
    // Doing this while the window is full-screen breaks the transition out of FS
    // (desktop and exclusive), so we must check for that before releasing mouse capture.
    if (!isStreamWindowFullscreen(m_Window) && !m_AbsoluteMouseMode) {
        setCaptureActive(false);
    }

    // A hold in progress never sees its key-up now, so it must not capture
    // the mouse again later
    m_MouseReleaseGesture.focusLost();
    m_MouseHoldReleased = m_MouseReleaseGesture.isPersistent();
    m_ShortcutMatcher.reset();

    // Raise all keys that are currently pressed. If we don't do this, certain keys
    // used in shortcuts that cause focus loss (such as Alt+Tab) may get stuck down.
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_Window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        ImmAssociateContext(info.info.win.window, NULL);
    }
#endif
}

void SdlInputHandler::holdReleaseMouse(bool held)
{
    if (held) {
        m_MouseHoldReleased = true;
        if (isCaptureActive()) {
            setCaptureActive(false);
        }
        return;
    }

    if (!m_MouseHoldReleased) {
        return;
    }
    m_MouseHoldReleased = false;
    if (m_Window != nullptr && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS)) {
        setCaptureActive(true);
    }
}

void SdlInputHandler::handleMouseReleaseHold(bool held)
{
    const bool wasFree = m_MouseReleaseGesture.isFree();
    const bool wasPersistent = m_MouseReleaseGesture.isPersistent();
    m_MouseReleaseGesture.hold(held, m_MouseReleaseClock.elapsed());
    applyMouseReleaseGesture(wasFree, wasPersistent);
}

bool SdlInputHandler::handleMouseReleaseEscape(SDL_KeyboardEvent* event)
{
    if (event->keysym.scancode != SDL_SCANCODE_ESCAPE) {
        return false;
    }
    const bool wasFree = m_MouseReleaseGesture.isFree();
    const bool wasPersistent = m_MouseReleaseGesture.isPersistent();
    const bool consumed = m_MouseReleaseGesture.escape(event->state == SDL_PRESSED);
    applyMouseReleaseGesture(wasFree, wasPersistent);
    return consumed;
}

void SdlInputHandler::applyMouseReleaseGesture(bool wasFree, bool wasPersistent)
{
    const bool persistent = m_MouseReleaseGesture.isPersistent();
    if (persistent != wasPersistent && m_MouseReleaseModeCallback) {
        // Close editors before restoring capture, and never let an editor
        // independently own capture while persistent mode is active.
        m_MouseReleaseModeCallback(persistent);
    }
    if (wasFree != m_MouseReleaseGesture.isFree() || (!wasPersistent && persistent)) {
        holdReleaseMouse(m_MouseReleaseGesture.isFree());
    }
}

bool SdlInputHandler::cancelMouseHoldRelease()
{
    if (m_MouseReleaseGesture.isPersistent()) {
        return false;
    }
    const bool wasReleased = m_MouseHoldReleased;
    m_MouseHoldReleased = false;
    return wasReleased;
}

bool SdlInputHandler::isCaptureActive()
{
    if (SDL_GetRelativeMouseMode()) {
        return true;
    }

    // Some platforms don't support SDL_SetRelativeMouseMode
    return m_FakeMouseCaptureActive;
}

bool SdlInputHandler::isSystemKeyCaptureEnabledForWindow()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN_OR_BORDERLESS) {
        Session* session = Session::get();
        return m_Window != nullptr &&
               (isStreamWindowFullscreen(m_Window) ||
                (session != nullptr && session->isStreamingWindowBorderlessOnDisplay()));
    }
    return m_Window != nullptr && m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF &&
           (m_CaptureSystemKeysMode != StreamingPreferences::CSK_FULLSCREEN ||
            isStreamWindowFullscreen(m_Window));
}

void SdlInputHandler::updateKeyboardGrabState()
{
    const bool shouldGrab = isSystemKeyCaptureEnabledForWindow() && isCaptureActive();

#if SDL_VERSION_ATLEAST(2, 0, 15)
    // On SDL 2.0.15+, we can get keyboard-only grab on Win32, X11, and Wayland.
    // SDL 2.0.18 adds keyboard grab on macOS (if built with non-AppStore APIs).
    SDL_SetWindowKeyboardGrab(m_Window, shouldGrab ? SDL_TRUE : SDL_FALSE);
#endif

    m_KeyboardCaptureActive = shouldGrab;
}

bool SdlInputHandler::isSystemKeyCaptureActive()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF) {
        return false;
    }

    if (m_Window == nullptr) {
        return false;
    }

    // NB: We used to check SDL_WINDOW_KEYBOARD_GRABBED here, but this isn't
    // always set when capture "fails" on SDL3, even though the user may have
    // configured the compositor to pass through system keys to us anyway.
    // See issues #1776 and #1900 for details.
    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
    if (!(windowFlags & SDL_WINDOW_INPUT_FOCUS) || !m_KeyboardCaptureActive) {
        return false;
    }

    return isSystemKeyCaptureEnabledForWindow();
}

void SdlInputHandler::setCaptureActive(bool active)
{
    if (active && m_MouseReleaseGesture.isFree()) {
        m_MouseHoldReleased = true;
        return;
    }
    if (active) {
        // If we're in relative mode, try to activate SDL's relative mouse mode
        if (m_AbsoluteMouseMode || SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
            m_FakeMouseCaptureActive = true;
            // Relative mouse mode didn't work or was disabled, so apply the
            // cursor shape and visibility directly.
            applyCapturedCursorState();
        }

        // Synchronize the client and host cursor when activating absolute capture
        if (m_AbsoluteMouseMode) {
            int mouseX, mouseY;
            int windowX, windowY;

            // We have to use SDL_GetGlobalMouseState() because macOS may not reflect
            // the new position of the mouse when outside the window.
            SDL_GetGlobalMouseState(&mouseX, &mouseY);

            // Convert global mouse state to window-relative
            SDL_GetWindowPosition(m_Window, &windowX, &windowY);
            mouseX -= windowX;
            mouseY -= windowY;

            if (isMouseInVideoRegion(mouseX, mouseY)) {
                // Synthesize a mouse event to synchronize the cursor
                SDL_MouseMotionEvent motionEvent = {};
                motionEvent.type = SDL_MOUSEMOTION;
                motionEvent.timestamp = SDL_GetTicks();
                motionEvent.windowID = SDL_GetWindowID(m_Window);
                motionEvent.x = mouseX;
                motionEvent.y = mouseY;
                handleMouseMotionEvent(&motionEvent);
            }
        }
    }
    else {
        // A capture transition can happen without a window focus event (for
        // example, when the user explicitly releases input). Ensure Sunshine
        // never retains contacts from the previous capture state.
        cancelNativeTouchpadContacts();

#ifdef HAVE_WINDOWS_PEN_INPUT
        cancelWindowsPenInput(true);
#endif

        if (m_RemoteCursor != nullptr && SDL_GetCursor() == m_RemoteCursor) {
            SDL_SetCursor(SDL_GetDefaultCursor());
        }

        if (m_FakeMouseCaptureActive) {
            // Display the cursor again
            SDL_ShowCursor(SDL_ENABLE);
            m_FakeMouseCaptureActive = false;
        }
        else {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}

void SdlInputHandler::handleTouchFingerEvent(SDL_TouchFingerEvent* event)
{
#ifdef HAS_QT_SDL_WAYLAND_BRIDGE
    SDL_TouchFingerEvent logicalEvent;
    if (m_WaylandCoordinateMetrics.has_value()) {
        logicalEvent = WaylandWindowMetrics::logicalTouchEvent(*event, *m_WaylandCoordinateMetrics);
        event = &logicalEvent;
    }
#endif

    // A stylus is a direct pointing device regardless of whether ordinary
    // touchscreen contacts are configured to emulate a trackpad.
    if (isPenTouchDevice(event->touchId)) {
        handleAbsoluteFingerEvent(event);
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 10)
    SDL_TouchDeviceType deviceType = SDL_GetTouchDeviceType(event->touchId);
    if (deviceType == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE && m_NativeTouchpadEnabled) {
        handleNativeTouchpadEvent(event);
        return;
    }
    else if (deviceType == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE && m_NativeTouchpadEnabled) {
        // Relative indirect devices don't provide physical surface coordinates,
        // so they cannot be represented by the native touchpad protocol.
        handleRelativeFingerEvent(event);
        return;
    }
    else if (deviceType != SDL_TOUCH_DEVICE_DIRECT) {
        // With native touchpad input disabled, SDL leaves trackpads on the
        // traditional mouse path. Ignore any other indirect touch callbacks.
        return;
    }
#elif defined(Q_OS_DARWIN)
    // SDL2 sends touch events from trackpads by default on
    // macOS. This totally screws our actual mouse handling,
    // so we must explicitly ignore touch events on macOS
    // until SDL 2.0.10 where we have SDL_GetTouchDeviceType()
    // to tell them apart.
    return;
#endif

    if (m_AbsoluteTouchMode) {
        handleAbsoluteFingerEvent(event);
    }
    else if (m_NativeTouchpadEnabled) {
        // A touchscreen in virtual trackpad mode provides the same normalized
        // contact data as an indirect absolute touchpad. Reuse the native
        // touchpad transport so the host can handle multi-touch gestures, with
        // the existing software pointer path as the compatibility fallback.
        handleNativeTouchpadEvent(event);
    }
    else {
        handleRelativeFingerEvent(event);
    }
}
