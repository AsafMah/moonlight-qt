#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"

#define VK_0 0x30
#define VK_A 0x41

// These are real Windows VK_* codes
#ifndef VK_F1
#define VK_F1 0x70
#define VK_F13 0x7C
#define VK_NUMPAD0 0x60
#endif

void SdlInputHandler::performSpecialKeyCombo(KeyCombo combo)
{
    switch (combo) {
    case KeyComboQuit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit key combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        break;

    case KeyComboUngrabInput:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse capture toggle combo");

        // Stop handling future input
        setCaptureActive(!isCaptureActive());

        // Force raise all keys to ensure they aren't stuck,
        // since we won't get their key up events.
        raiseAllKeys();
        break;

    case KeyComboToggleFullScreen:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected full-screen toggle combo");
        Session::s_ActiveSession->toggleFullscreen();

        // Force raise all keys just be safe across this full-screen/windowed
        // transition just in case key events get lost.
        raiseAllKeys();
        break;

    case KeyComboToggleStatsOverlay:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle combo");

        // Toggle the stats overlay
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));
        break;

    case KeyComboToggleMouseMode:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse mode toggle combo");

        // Uncapture input
        setCaptureActive(false);

        // Toggle mouse mode
        m_AbsoluteMouseMode = !m_AbsoluteMouseMode;

        // Recapture input
        setCaptureActive(true);
        break;

    case KeyComboToggleCursorHide:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected show mouse combo");

        if (!SDL_GetRelativeMouseMode()) {
            m_MouseCursorCapturedVisibilityState = !m_MouseCursorCapturedVisibilityState;
            SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Cursor can only be shown in remote desktop mouse mode");
        }
        break;

    case KeyComboToggleMinimize:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected minimize combo");
        SDL_MinimizeWindow(m_Window);
        break;

    case KeyComboPasteText:
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected type clipboard text combo");

        // Force raise all keys to ensure that none of them interfere
        // with the text we're going to type.
        raiseAllKeys();

        char* text;
        if (SDL_HasClipboardText() && (text = SDL_GetClipboardText()) != nullptr) {
            // Sending both CR and LF will lead to two newlines in the destination for
            // each newline in the source, so we fix up any CRLFs into just a single LF.
            for (char* c = text; *c != 0; c++) {
                if (*c == '\r' && *(c + 1) == '\n') {
                    // We're using strlen() rather than strlen() - 1 since we need to add 1
                    // to copy the null terminator which is not included in strlen()'s count.
                    memmove(c, c + 1, strlen(c));
                }
            }

            // Send this text to the PC
            LiSendUtf8TextEvent(text, (unsigned int)strlen(text));

            // SDL_GetClipboardText() allocates, so we must free
            SDL_free((void*)text);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No text in clipboard to paste!");
        }
        break;
    }

    case KeyComboTogglePointerRegionLock:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected pointer region lock toggle combo");
        m_PointerRegionLockActive = !m_PointerRegionLockActive;

        // Remember that the user changed this manually, so we don't mess with it anymore
        // during windowed <-> full-screen transitions.
        m_PointerRegionLockToggledByUser = true;

        // Apply the new region lock
        updatePointerRegionLock();
        break;

    case KeyComboQuitAndExit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quitAndExit key combo");

        // Indicate that we want to exit afterwards
        Session::get()->setShouldExit(true);

        // Push a quit event to the main loop
        SDL_Event quitExitEvent;
        quitExitEvent.type = SDL_QUIT;
        quitExitEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&quitExitEvent);
        break;

    case KeyComboToggleKeyboardGrab:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected keyboard grab toggle combo");

        // Toggle the system key capture mode
        if (isSystemKeyCaptureActive()) {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_OFF;
        }
        else {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
        }

        updateKeyboardGrabState();
        break;

    default:
        Q_UNREACHABLE();
    }
}

// Shortcut bindings store SDL2 scancodes without depending on SDL
static constexpr bool isShortcutScancode(SDL_Scancode sdlScancode, int shortcutScancode)
{
    return static_cast<int>(sdlScancode) == shortcutScancode;
}
static_assert(isShortcutScancode(SDL_SCANCODE_A, ShortcutKeys::A) &&
                  isShortcutScancode(SDL_SCANCODE_Z, ShortcutKeys::Z) &&
                  isShortcutScancode(SDL_SCANCODE_1, ShortcutKeys::Num1) &&
                  isShortcutScancode(SDL_SCANCODE_0, ShortcutKeys::Num0) &&
                  isShortcutScancode(SDL_SCANCODE_F1, ShortcutKeys::F1) &&
                  isShortcutScancode(SDL_SCANCODE_F12, ShortcutKeys::F12) &&
                  isShortcutScancode(SDL_SCANCODE_F13, ShortcutKeys::F13) &&
                  isShortcutScancode(SDL_SCANCODE_F24, ShortcutKeys::F24) &&
                  isShortcutScancode(SDL_SCANCODE_MUTE, ShortcutKeys::Mute) &&
                  isShortcutScancode(SDL_SCANCODE_VOLUMEUP, ShortcutKeys::VolumeUp) &&
                  isShortcutScancode(SDL_SCANCODE_VOLUMEDOWN, ShortcutKeys::VolumeDown) &&
                  isShortcutScancode(SDL_SCANCODE_LCTRL, ShortcutKeys::LeftCtrl) &&
                  isShortcutScancode(SDL_SCANCODE_RGUI, ShortcutKeys::RightGui) &&
                  isShortcutScancode(SDL_SCANCODE_AUDIONEXT, ShortcutKeys::AudioNext) &&
                  isShortcutScancode(SDL_SCANCODE_AUDIOPREV, ShortcutKeys::AudioPrev) &&
                  isShortcutScancode(SDL_SCANCODE_AUDIOSTOP, ShortcutKeys::AudioStop) &&
                  isShortcutScancode(SDL_SCANCODE_AUDIOPLAY, ShortcutKeys::AudioPlay) &&
                  isShortcutScancode(SDL_SCANCODE_AUDIOMUTE, ShortcutKeys::AudioMute),
              "Shortcut scancodes must match SDL");
static_assert(static_cast<int>(SDL_NUM_SCANCODES) <= ShortcutKeys::MaxScancode,
              "Shortcut scancodes must fit");

static quint8 shortcutModifiers(Uint16 mod)
{
    quint8 modifiers = ShortcutBinding::ModNone;
    if (mod & KMOD_CTRL) {
        modifiers |= ShortcutBinding::ModCtrl;
    }
    if (mod & KMOD_ALT) {
        modifiers |= ShortcutBinding::ModAlt;
    }
    if (mod & KMOD_SHIFT) {
        modifiers |= ShortcutBinding::ModShift;
    }
    if (mod & KMOD_GUI) {
        modifiers |= ShortcutBinding::ModGui;
    }
    return modifiers;
}

// US layout scancode of the latin letter or digit a key types, if any
static int shortcutLatinKey(SDL_Keycode sym)
{
    if (sym >= SDLK_a && sym <= SDLK_z) {
        return ShortcutKeys::A + (sym - SDLK_a);
    }
    else if (sym >= SDLK_1 && sym <= SDLK_9) {
        return ShortcutKeys::Num1 + (sym - SDLK_1);
    }
    else if (sym == SDLK_0) {
        return ShortcutKeys::Num0;
    }
    return ShortcutKeys::Unknown;
}

void SdlInputHandler::reloadShortcuts()
{
    m_ShortcutMatcher.setProfile(ShortcutProfile::loadFromSettings());
}

void SdlInputHandler::handleKeyEvent(SDL_KeyboardEvent* event)
{
    short keyCode;
    char modifiers;
    bool shouldNotConvertToScanCodeOnServer = false;

    if (event->repeat) {
        // Ignore repeat key down events
        SDL_assert(event->state == SDL_PRESSED);
        return;
    }

    // Check for keyboard shortcuts. As before, letters and digits match the
    // key the user sees on their keyboard (keysym) first, and the scancode only
    // if nothing matched, so non-latin layouts still have working shortcuts.
    // The exact set of modifiers must match, and both the key-down and key-up
    // of a shortcut key are kept from the host.
    if (m_ShortcutMatcher.processKeyEvent(event->keysym.scancode,
                                          shortcutLatinKey(event->keysym.sym),
                                          shortcutModifiers(event->keysym.mod),
                                          event->state == SDL_PRESSED, event->repeat != 0,
                                          m_ShortcutDispatcher)) {
        return;
    }

    // Set modifier flags
    modifiers = 0;
    if (event->keysym.mod & KMOD_CTRL) {
        modifiers |= MODIFIER_CTRL;
    }
    if (event->keysym.mod & KMOD_ALT) {
        modifiers |= MODIFIER_ALT;
    }
    if (event->keysym.mod & KMOD_SHIFT) {
        modifiers |= MODIFIER_SHIFT;
    }
    if (event->keysym.mod & KMOD_GUI) {
        if (isSystemKeyCaptureActive()) {
            modifiers |= MODIFIER_META;
        }
    }

    // Set keycode. We explicitly use scancode here because GFE will try to correct
    // for AZERTY layouts on the host but it depends on receiving VK_ values matching
    // a QWERTY layout to work.
    if (event->keysym.scancode >= SDL_SCANCODE_1 && event->keysym.scancode <= SDL_SCANCODE_9) {
        // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_1) + VK_0 + 1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_A && event->keysym.scancode <= SDL_SCANCODE_Z) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_A) + VK_A;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F1 && event->keysym.scancode <= SDL_SCANCODE_F12) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F1) + VK_F1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F13 && event->keysym.scancode <= SDL_SCANCODE_F24) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F13) + VK_F13;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_KP_1 && event->keysym.scancode <= SDL_SCANCODE_KP_9) {
        // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1;
    }
    else {
        switch (event->keysym.scancode) {
            case SDL_SCANCODE_BACKSPACE:
                keyCode = 0x08;
                break;
            case SDL_SCANCODE_TAB:
                keyCode = 0x09;
                break;
            case SDL_SCANCODE_CLEAR:
                keyCode = 0x0C;
                break;
            case SDL_SCANCODE_KP_ENTER: // FIXME: Is this correct?
            case SDL_SCANCODE_RETURN:
                keyCode = 0x0D;
                break;
            case SDL_SCANCODE_PAUSE:
                keyCode = 0x13;
                break;
            case SDL_SCANCODE_CAPSLOCK:
                keyCode = 0x14;
                break;
            case SDL_SCANCODE_ESCAPE:
                keyCode = 0x1B;
                break;
            case SDL_SCANCODE_SPACE:
                keyCode = 0x20;
                break;
            case SDL_SCANCODE_PAGEUP:
                keyCode = 0x21;
                break;
            case SDL_SCANCODE_PAGEDOWN:
                keyCode = 0x22;
                break;
            case SDL_SCANCODE_END:
                keyCode = 0x23;
                break;
            case SDL_SCANCODE_HOME:
                keyCode = 0x24;
                break;
            case SDL_SCANCODE_LEFT:
                keyCode = 0x25;
                break;
            case SDL_SCANCODE_UP:
                keyCode = 0x26;
                break;
            case SDL_SCANCODE_RIGHT:
                keyCode = 0x27;
                break;
            case SDL_SCANCODE_DOWN:
                keyCode = 0x28;
                break;
            case SDL_SCANCODE_SELECT:
                keyCode = 0x29;
                break;
            case SDL_SCANCODE_EXECUTE:
                keyCode = 0x2B;
                break;
            case SDL_SCANCODE_PRINTSCREEN:
                keyCode = 0x2C;
                break;
            case SDL_SCANCODE_INSERT:
                keyCode = 0x2D;
                break;
            case SDL_SCANCODE_DELETE:
                keyCode = 0x2E;
                break;
            case SDL_SCANCODE_HELP:
                keyCode = 0x2F;
                break;
            case SDL_SCANCODE_KP_0:
                // See comment above about why we only handle SDL_SCANCODE_KP_0 here
                keyCode = VK_NUMPAD0;
                break;
            case SDL_SCANCODE_0:
                // See comment above about why we only handle SDL_SCANCODE_0 here
                keyCode = VK_0;
                break;
            case SDL_SCANCODE_KP_MULTIPLY:
                keyCode = 0x6A;
                break;
            case SDL_SCANCODE_KP_PLUS:
                keyCode = 0x6B;
                break;
            case SDL_SCANCODE_KP_COMMA:
                keyCode = 0x6C;
                break;
            case SDL_SCANCODE_KP_MINUS:
                keyCode = 0x6D;
                break;
            case SDL_SCANCODE_KP_PERIOD:
                keyCode = 0x6E;
                break;
            case SDL_SCANCODE_KP_DIVIDE:
                keyCode = 0x6F;
                break;
            case SDL_SCANCODE_NUMLOCKCLEAR:
                keyCode = 0x90;
                break;
            case SDL_SCANCODE_SCROLLLOCK:
                keyCode = 0x91;
                break;
            case SDL_SCANCODE_LSHIFT:
                keyCode = 0xA0;
                break;
            case SDL_SCANCODE_RSHIFT:
                keyCode = 0xA1;
                break;
            case SDL_SCANCODE_LCTRL:
                keyCode = 0xA2;
                break;
            case SDL_SCANCODE_RCTRL:
                keyCode = 0xA3;
                break;
            case SDL_SCANCODE_LALT:
                keyCode = 0xA4;
                break;
            case SDL_SCANCODE_RALT:
                keyCode = 0xA5;
                break;
            case SDL_SCANCODE_LGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = 0x5B;
                break;
            case SDL_SCANCODE_RGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = 0x5C;
                break;
            case SDL_SCANCODE_APPLICATION:
                keyCode = 0x5D;
                break;
            case SDL_SCANCODE_AC_BACK:
                keyCode = 0xA6;
                break;
            case SDL_SCANCODE_AC_FORWARD:
                keyCode = 0xA7;
                break;
            case SDL_SCANCODE_AC_REFRESH:
                keyCode = 0xA8;
                break;
            case SDL_SCANCODE_AC_STOP:
                keyCode = 0xA9;
                break;
            case SDL_SCANCODE_AC_SEARCH:
                keyCode = 0xAA;
                break;
            case SDL_SCANCODE_AC_BOOKMARKS:
                keyCode = 0xAB;
                break;
            case SDL_SCANCODE_AC_HOME:
                keyCode = 0xAC;
                break;
            case SDL_SCANCODE_SEMICOLON:
                keyCode = 0xBA;
                break;
            case SDL_SCANCODE_EQUALS:
                keyCode = 0xBB;
                break;
            case SDL_SCANCODE_COMMA:
                keyCode = 0xBC;
                break;
            case SDL_SCANCODE_MINUS:
                keyCode = 0xBD;
                break;
            case SDL_SCANCODE_PERIOD:
                keyCode = 0xBE;
                break;
            case SDL_SCANCODE_SLASH:
                keyCode = 0xBF;
                break;
            case SDL_SCANCODE_GRAVE:
                keyCode = 0xC0;
                break;
            case SDL_SCANCODE_LEFTBRACKET:
                keyCode = 0xDB;
                break;
            case SDL_SCANCODE_INTERNATIONAL3:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_BACKSLASH:
                keyCode = 0xDC;
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
                keyCode = 0xDD;
                break;
            case SDL_SCANCODE_APOSTROPHE:
                keyCode = 0xDE;
                break;
            case SDL_SCANCODE_INTERNATIONAL1:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_NONUSBACKSLASH:
                keyCode = 0xE2;
                break;
            case SDL_SCANCODE_LANG1:
                keyCode = 0x1C;
                break;
            case SDL_SCANCODE_LANG2:
                keyCode = 0x1D;
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled button event: %d",
                             event->keysym.scancode);
                return;
        }
    }

    // Track the key state so we always know which keys are down
    if (event->state == SDL_PRESSED) {
        m_KeysDown.insert(keyCode);
    }
    else {
        m_KeysDown.remove(keyCode);
    }

    LiSendKeyboardEvent2(0x8000 | keyCode,
                        event->state == SDL_PRESSED ?
                            KEY_ACTION_DOWN : KEY_ACTION_UP,
                        modifiers,
                        shouldNotConvertToScanCodeOnServer ? SS_KBE_FLAG_NON_NORMALIZED : 0);
}
