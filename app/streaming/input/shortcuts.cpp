#include "shortcuts.h"

#include <QCoreApplication>
#include <QSettings>

#define SER_SHORTCUTS "shortcuts"
#define SER_DEFAULTPREFIX "defaultPrefix"
#define SER_BINDINGS "bindings"
#define SER_RIGHT_ALT_RELEASE "right-alt-release-enabled"

namespace {

struct NamedKey
{
    int scancode;
    const char* token;
    const char* display;
};

// Keys other than letters, digits, F1-F24 and the keypad digits, which are
// generated. Tokens are persisted, so never change an existing one.
const NamedKey k_NamedKeys[] = {
    { 40, "Enter", "Enter" },
    { 41, "Escape", "Esc" },
    { 42, "Backspace", "Backspace" },
    { 43, "Tab", "Tab" },
    { 44, "Space", "Space" },
    { 45, "Minus", "-" },
    { 46, "Equals", "=" },
    { 47, "LeftBracket", "[" },
    { 48, "RightBracket", "]" },
    { 49, "Backslash", "\\" },
    { 50, "NonUsHash", "#" },
    { 51, "Semicolon", ";" },
    { 52, "Apostrophe", "'" },
    { 53, "Grave", "`" },
    { 54, "Comma", "," },
    { 55, "Period", "." },
    { 56, "Slash", "/" },
    { 57, "CapsLock", "Caps Lock" },
    { 70, "PrintScreen", "Print Screen" },
    { 71, "ScrollLock", "Scroll Lock" },
    { 72, "Pause", "Pause" },
    { 73, "Insert", "Insert" },
    { 74, "Home", "Home" },
    { 75, "PageUp", "Page Up" },
    { 76, "Delete", "Delete" },
    { 77, "End", "End" },
    { 78, "PageDown", "Page Down" },
    { 79, "Right", "Right" },
    { 80, "Left", "Left" },
    { 81, "Down", "Down" },
    { 82, "Up", "Up" },
    { 83, "NumLock", "Num Lock" },
    { 84, "KpDivide", "Num /" },
    { 85, "KpMultiply", "Num *" },
    { 86, "KpMinus", "Num -" },
    { 87, "KpPlus", "Num +" },
    { 88, "KpEnter", "Num Enter" },
    { 98, "Kp0", "Num 0" },
    { 99, "KpPeriod", "Num ." },
    { 100, "NonUsBackslash", "\\" },
    { 101, "Application", "Menu" },
    { 103, "KpEquals", "Num =" },
    { ShortcutKeys::LeftCtrl, "LeftCtrl", "Left Ctrl" },
    { ShortcutKeys::LeftShift, "LeftShift", "Left Shift" },
    { ShortcutKeys::LeftAlt, "LeftAlt", "Left Alt" },
    { ShortcutKeys::LeftGui, "LeftGui", "Left Win" },
    { ShortcutKeys::RightCtrl, "RightCtrl", "Right Ctrl" },
    { ShortcutKeys::RightShift, "RightShift", "Right Shift" },
    { ShortcutKeys::RightAlt, "RightAlt", "Right Alt" },
    { ShortcutKeys::RightGui, "RightGui", "Right Win" },
    { ShortcutKeys::Mute, "Mute", "Mute" },
    { ShortcutKeys::VolumeUp, "VolumeUp", "Volume Up" },
    { ShortcutKeys::VolumeDown, "VolumeDown", "Volume Down" },
    { ShortcutKeys::AudioNext, "MediaNext", "Next Track" },
    { ShortcutKeys::AudioPrev, "MediaPrevious", "Previous Track" },
    { ShortcutKeys::AudioStop, "MediaStop", "Stop" },
    { ShortcutKeys::AudioPlay, "MediaPlay", "Play/Pause" },
    { ShortcutKeys::AudioMute, "MediaMute", "Mute" },
};

const NamedKey* findNamedKey(int scancode)
{
    for (const NamedKey& key : k_NamedKeys) {
        if (key.scancode == scancode) {
            return &key;
        }
    }
    return nullptr;
}

// Display name of a modifier key in the platform's own words
QString modifierKeyDisplay(int scancode)
{
    const bool left = scancode < ShortcutKeys::RightCtrl;
    QString name;
    switch (ShortcutBinding::modifierForKey(scancode)) {
    case ShortcutBinding::ModCtrl:
        name = QStringLiteral("Ctrl");
        break;
    case ShortcutBinding::ModShift:
        name = QStringLiteral("Shift");
        break;
    case ShortcutBinding::ModAlt:
#if defined(Q_OS_DARWIN)
        name = QStringLiteral("Option");
#else
        name = QStringLiteral("Alt");
#endif
        break;
    case ShortcutBinding::ModGui:
#if defined(Q_OS_DARWIN)
        name = QStringLiteral("Cmd");
#elif defined(Q_OS_WIN32)
        name = QStringLiteral("Win");
#else
        name = QStringLiteral("Super");
#endif
        break;
    default:
        return QString();
    }
    return (left ? QStringLiteral("Left ") : QStringLiteral("Right ")) + name;
}

// Generated names: letters, digits, F1-F24 and keypad 1-9 (keypad 0 is named)
QString generatedToken(int scancode)
{
    if (scancode >= ShortcutKeys::A && scancode <= ShortcutKeys::Z) {
        return QString(QChar('A' + (scancode - ShortcutKeys::A)));
    } else if (scancode >= ShortcutKeys::Num1 && scancode < ShortcutKeys::Num0) {
        return QString::number(1 + (scancode - ShortcutKeys::Num1));
    } else if (scancode == ShortcutKeys::Num0) {
        return QStringLiteral("0");
    } else if (scancode >= ShortcutKeys::F1 && scancode <= ShortcutKeys::F12) {
        return QStringLiteral("F%1").arg(1 + (scancode - ShortcutKeys::F1));
    } else if (scancode >= ShortcutKeys::F13 && scancode <= ShortcutKeys::F24) {
        return QStringLiteral("F%1").arg(13 + (scancode - ShortcutKeys::F13));
    } else if (scancode >= 89 && scancode <= 97) {
        return QStringLiteral("Kp%1").arg(1 + (scancode - 89));
    }
    return QString();
}

struct NativeKey
{
    quint32 native;
    int scancode;
};

#if defined(Q_OS_WIN32) || (defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN))
// PC scan code set 1 for 0x01-0x58. Linux evdev key codes use the same values
// for this range, except 0x45 (Num Lock on Linux, Pause on Windows).
const int k_PcBaseScancodes[] = {
    0,  41, 30, 31, 32, 33, 34,  35, 36,  37, 38,  39, 45, 46,  42, 43, // 0x00
    20, 26, 8,  21, 23, 28, 24,  12, 18,  19, 47,  48, 40, 224, 4,  22, // 0x10
    7,  9,  10, 11, 13, 14, 15,  51, 52,  53, 225, 49, 29, 27,  6,  25, // 0x20
    5,  17, 16, 54, 55, 56, 229, 85, 226, 44, 57,  58, 59, 60,  61, 62, // 0x30
    63, 64, 65, 66, 67, 83, 71,  95, 96,  97, 86,  92, 93, 94,  87, 89, // 0x40
    90, 91, 98, 99, 0,  0,  100, 68, 69,                                // 0x50
};

int fromPcBaseScancode(quint32 code)
{
    if (code < sizeof(k_PcBaseScancodes) / sizeof(k_PcBaseScancodes[0])) {
        return k_PcBaseScancodes[code];
    }
    return ShortcutKeys::Unknown;
}
#endif

int lookupNative(const NativeKey* table, int count, quint32 native)
{
    for (int i = 0; i < count; i++) {
        if (table[i].native == native) {
            return table[i].scancode;
        }
    }
    return ShortcutKeys::Unknown;
}

#if defined(Q_OS_WIN32)
// Extended (E0-prefixed) keys. Qt reports them with bit 8 set.
const NativeKey k_WindowsExtendedKeys[] = {
    { 0x10, ShortcutKeys::AudioPrev },
    { 0x19, ShortcutKeys::AudioNext },
    { 0x1C, 88 },
    { 0x1D, 228 },
    { 0x20, ShortcutKeys::AudioMute },
    { 0x22, ShortcutKeys::AudioPlay },
    { 0x24, ShortcutKeys::AudioStop },
    { 0x2E, ShortcutKeys::VolumeDown },
    { 0x30, ShortcutKeys::VolumeUp },
    { 0x35, 84 },
    { 0x37, 70 },
    { 0x38, 230 },
    { 0x45, 83 },
    { 0x47, 74 },
    { 0x48, 82 },
    { 0x49, 75 },
    { 0x4B, 80 },
    { 0x4D, 79 },
    { 0x4F, 77 },
    { 0x50, 81 },
    { 0x51, 78 },
    { 0x52, 73 },
    { 0x53, 76 },
    { 0x5B, 227 },
    { 0x5C, 231 },
    { 0x5D, 101 },
};

int fromNativeScanCode(quint32 native)
{
    if (native & 0x100) {
        return lookupNative(k_WindowsExtendedKeys,
                            int(sizeof(k_WindowsExtendedKeys) / sizeof(k_WindowsExtendedKeys[0])),
                            native & 0xFF);
    } else if (native == 0x45) {
        return 72; // Pause
    } else if (native >= 0x64 && native <= 0x6E) {
        return ShortcutKeys::F13 + int(native - 0x64);
    } else if (native == 0x76) {
        return ShortcutKeys::F24;
    }
    return fromPcBaseScancode(native);
}
#elif defined(Q_OS_DARWIN)
// macOS virtual key codes (kVK_*). kVK_ANSI_A is 0, which can't be told apart
// from a missing native code, so A goes through the Qt::Key fallback.
const NativeKey k_MacKeys[] = {
    { 0x01, 22 },  { 0x02, 7 },   { 0x03, 9 },   { 0x04, 11 },  { 0x05, 10 },  { 0x06, 29 },
    { 0x07, 27 },  { 0x08, 6 },   { 0x09, 25 },  { 0x0B, 5 },   { 0x0C, 20 },  { 0x0D, 26 },
    { 0x0E, 8 },   { 0x0F, 21 },  { 0x10, 28 },  { 0x11, 23 },  { 0x12, 30 },  { 0x13, 31 },
    { 0x14, 32 },  { 0x15, 33 },  { 0x16, 35 },  { 0x17, 34 },  { 0x18, 46 },  { 0x19, 38 },
    { 0x1A, 36 },  { 0x1B, 45 },  { 0x1C, 37 },  { 0x1D, 39 },  { 0x1E, 48 },  { 0x1F, 18 },
    { 0x20, 24 },  { 0x21, 47 },  { 0x22, 12 },  { 0x23, 19 },  { 0x24, 40 },  { 0x25, 15 },
    { 0x26, 13 },  { 0x27, 52 },  { 0x28, 14 },  { 0x29, 51 },  { 0x2A, 49 },  { 0x2B, 54 },
    { 0x2C, 56 },  { 0x2D, 17 },  { 0x2E, 16 },  { 0x2F, 55 },  { 0x30, 43 },  { 0x31, 44 },
    { 0x32, 53 },  { 0x33, 42 },  { 0x35, 41 },  { 0x40, 108 }, { 0x41, 99 },  { 0x43, 85 },
    { 0x45, 87 },  { 0x47, 83 },  { 0x48, 128 }, { 0x49, 129 }, { 0x4A, 127 }, { 0x4B, 84 },
    { 0x4C, 88 },  { 0x4E, 86 },  { 0x4F, 109 }, { 0x50, 110 }, { 0x51, 103 }, { 0x52, 98 },
    { 0x53, 89 },  { 0x54, 90 },  { 0x55, 91 },  { 0x56, 92 },  { 0x57, 93 },  { 0x58, 94 },
    { 0x59, 95 },  { 0x5A, 111 }, { 0x5B, 96 },  { 0x5C, 97 },  { 0x60, 62 },  { 0x61, 63 },
    { 0x62, 64 },  { 0x63, 60 },  { 0x64, 65 },  { 0x65, 66 },  { 0x67, 68 },  { 0x69, 104 },
    { 0x6A, 107 }, { 0x6B, 105 }, { 0x6D, 67 },  { 0x6F, 69 },  { 0x71, 106 }, { 0x72, 73 },
    { 0x73, 74 },  { 0x74, 75 },  { 0x75, 76 },  { 0x76, 61 },  { 0x77, 77 },  { 0x78, 59 },
    { 0x79, 78 },  { 0x7A, 58 },  { 0x7B, 80 },  { 0x7C, 79 },  { 0x7D, 81 },  { 0x7E, 82 },
};

// Modifier keys, for hold actions bound to one of them
const NativeKey k_MacModifierKeys[] = {
    { 0x36, 231 }, { 0x37, 227 }, { 0x38, 225 }, { 0x3A, 226 },
    { 0x3B, 224 }, { 0x3C, 229 }, { 0x3D, 230 }, { 0x3E, 228 },
};

int fromNativeScanCode(quint32 native)
{
    const int scancode =
        lookupNative(k_MacKeys, int(sizeof(k_MacKeys) / sizeof(k_MacKeys[0])), native);
    if (scancode != ShortcutKeys::Unknown) {
        return scancode;
    }
    return lookupNative(k_MacModifierKeys,
                        int(sizeof(k_MacModifierKeys) / sizeof(k_MacModifierKeys[0])), native);
}
#elif defined(Q_OS_UNIX)
// X11, Wayland and libinput report evdev key codes offset by 8
const NativeKey k_EvdevKeys[] = {
    { 96, 88 },
    { 97, 228 },
    { 98, 84 },
    { 99, 70 },
    { 100, 230 },
    { 102, 74 },
    { 103, 82 },
    { 104, 75 },
    { 105, 80 },
    { 106, 79 },
    { 107, 77 },
    { 108, 81 },
    { 109, 78 },
    { 110, 73 },
    { 111, 76 },
    { 113, ShortcutKeys::Mute },
    { 114, ShortcutKeys::VolumeDown },
    { 115, ShortcutKeys::VolumeUp },
    { 117, 103 },
    { 119, 72 },
    { 125, 227 },
    { 126, 231 },
    { 127, 101 },
    { 163, ShortcutKeys::AudioNext },
    { 164, ShortcutKeys::AudioPlay },
    { 165, ShortcutKeys::AudioPrev },
    { 166, ShortcutKeys::AudioStop },
};

int fromNativeScanCode(quint32 native)
{
    if (native <= 8) {
        return ShortcutKeys::Unknown;
    }

    const quint32 evdev = native - 8;
    if (evdev >= 183 && evdev <= 194) {
        return ShortcutKeys::F13 + int(evdev - 183);
    }

    const int scancode = fromPcBaseScancode(evdev);
    if (scancode != ShortcutKeys::Unknown) {
        return scancode;
    }
    return lookupNative(k_EvdevKeys, int(sizeof(k_EvdevKeys) / sizeof(k_EvdevKeys[0])), evdev);
}
#else
int fromNativeScanCode(quint32)
{
    return ShortcutKeys::Unknown;
}
#endif

// Qt::Key to US layout position, for when no usable native code is available
const NativeKey k_QtKeys[] = {
    { Qt::Key_Return, 40 },
    { Qt::Key_Escape, 41 },
    { Qt::Key_Backspace, 42 },
    { Qt::Key_Tab, 43 },
    { Qt::Key_Backtab, 43 },
    { Qt::Key_Space, 44 },
    { Qt::Key_Minus, 45 },
    { Qt::Key_Underscore, 45 },
    { Qt::Key_Equal, 46 },
    { Qt::Key_Plus, 46 },
    { Qt::Key_BracketLeft, 47 },
    { Qt::Key_BraceLeft, 47 },
    { Qt::Key_BracketRight, 48 },
    { Qt::Key_BraceRight, 48 },
    { Qt::Key_Backslash, 49 },
    { Qt::Key_Bar, 49 },
    { Qt::Key_Semicolon, 51 },
    { Qt::Key_Colon, 51 },
    { Qt::Key_Apostrophe, 52 },
    { Qt::Key_QuoteDbl, 52 },
    { Qt::Key_QuoteLeft, 53 },
    { Qt::Key_AsciiTilde, 53 },
    { Qt::Key_Comma, 54 },
    { Qt::Key_Less, 54 },
    { Qt::Key_Period, 55 },
    { Qt::Key_Greater, 55 },
    { Qt::Key_Slash, 56 },
    { Qt::Key_Question, 56 },
    { Qt::Key_Exclam, 30 },
    { Qt::Key_At, 31 },
    { Qt::Key_NumberSign, 32 },
    { Qt::Key_Dollar, 33 },
    { Qt::Key_Percent, 34 },
    { Qt::Key_AsciiCircum, 35 },
    { Qt::Key_Ampersand, 36 },
    { Qt::Key_Asterisk, 37 },
    { Qt::Key_ParenLeft, 38 },
    { Qt::Key_ParenRight, 39 },
    { Qt::Key_CapsLock, 57 },
    { Qt::Key_Print, 70 },
    { Qt::Key_ScrollLock, 71 },
    { Qt::Key_Pause, 72 },
    { Qt::Key_Insert, 73 },
    { Qt::Key_Home, 74 },
    { Qt::Key_PageUp, 75 },
    { Qt::Key_Delete, 76 },
    { Qt::Key_End, 77 },
    { Qt::Key_PageDown, 78 },
    { Qt::Key_Right, 79 },
    { Qt::Key_Left, 80 },
    { Qt::Key_Down, 81 },
    { Qt::Key_Up, 82 },
    { Qt::Key_NumLock, 83 },
    { Qt::Key_Enter, 88 },
    { Qt::Key_Menu, 101 },
    { Qt::Key_VolumeMute, ShortcutKeys::Mute },
    { Qt::Key_VolumeUp, ShortcutKeys::VolumeUp },
    { Qt::Key_VolumeDown, ShortcutKeys::VolumeDown },
    { Qt::Key_MediaNext, ShortcutKeys::AudioNext },
    { Qt::Key_MediaPrevious, ShortcutKeys::AudioPrev },
    { Qt::Key_MediaStop, ShortcutKeys::AudioStop },
    { Qt::Key_MediaPlay, ShortcutKeys::AudioPlay },
    { Qt::Key_MediaTogglePlayPause, ShortcutKeys::AudioPlay },
};

int fromQtKey(int qtKey)
{
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return ShortcutKeys::A + (qtKey - Qt::Key_A);
    } else if (qtKey >= Qt::Key_1 && qtKey <= Qt::Key_9) {
        return ShortcutKeys::Num1 + (qtKey - Qt::Key_1);
    } else if (qtKey == Qt::Key_0) {
        return ShortcutKeys::Num0;
    } else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12) {
        return ShortcutKeys::F1 + (qtKey - Qt::Key_F1);
    } else if (qtKey >= Qt::Key_F13 && qtKey <= Qt::Key_F24) {
        return ShortcutKeys::F13 + (qtKey - Qt::Key_F13);
    }
    return lookupNative(k_QtKeys, int(sizeof(k_QtKeys) / sizeof(k_QtKeys[0])), quint32(qtKey));
}

const struct
{
    quint8 modifier;
    const char* token;
} k_ModifierTokens[] = {
    { ShortcutBinding::ModCtrl, "ctrl" },
    { ShortcutBinding::ModAlt, "alt" },
    { ShortcutBinding::ModShift, "shift" },
    { ShortcutBinding::ModGui, "gui" },
};

quint8 parseModifierToken(const QString& token)
{
    const QString lower = token.toLower();
    for (const auto& mod : k_ModifierTokens) {
        if (lower == QLatin1String(mod.token)) {
            return mod.modifier;
        }
    }
    if (lower == QLatin1String("control")) {
        return ShortcutBinding::ModCtrl;
    } else if (lower == QLatin1String("meta") || lower == QLatin1String("win") ||
               lower == QLatin1String("cmd") || lower == QLatin1String("super")) {
        return ShortcutBinding::ModGui;
    }
    return ShortcutBinding::ModNone;
}

const char* const k_PrefixTokens[] = {
    "ctrl+alt+shift", // PrefixCtrlAltShift
    "ctrl+alt",       // PrefixCtrlAlt
};

ShortcutAction makeAction(const char* id, const char* name, const char* group, char key)
{
    return ShortcutAction{ QString::fromLatin1(id),
                           name,
                           group,
                           { { true, ShortcutBinding::ModNone, ShortcutKeys::letter(key) } } };
}

// An action that is off until the user binds a key to it
ShortcutAction makeUnboundAction(const char* id, const char* name, const char* group)
{
    return ShortcutAction{ QString::fromLatin1(id), name, group, {} };
}
// A hold action, unbound by default
ShortcutAction makeHoldAction(const char* id, const char* name, const char* group)
{
    return ShortcutAction{ QString::fromLatin1(id), name, group, {}, true };
}

QVector<ShortcutAction> builtinActions()
{
    // Default keys are the historical Ctrl+Alt+Shift combos
    return {
        makeAction(ShortcutActions::Quit, QT_TRANSLATE_NOOP("Shortcuts", "Disconnect from host"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Session"), 'Q'),
        makeAction(ShortcutActions::QuitAndExit,
                   QT_TRANSLATE_NOOP("Shortcuts", "Disconnect and quit Moonlight"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Session"), 'E'),
        makeAction(ShortcutActions::ToggleFullScreen,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle full-screen"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Window"), 'X'),
        makeAction(ShortcutActions::Minimize, QT_TRANSLATE_NOOP("Shortcuts", "Minimize window"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Window"), 'D'),
        makeAction(ShortcutActions::ToggleStatsOverlay,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle performance stats"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Window"), 'S'),
        makeAction(ShortcutActions::ToggleOverlayMenu,
                   QT_TRANSLATE_NOOP("Shortcuts", "Show or hide the overlay menu"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Window"), 'O'),
        makeAction(ShortcutActions::CycleOverlayPreset,
                   QT_TRANSLATE_NOOP("Shortcuts", "Cycle performance overlay presets"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Window"), 'P'),
        ShortcutAction{ QString::fromLatin1(ShortcutActions::EditOverlayPosition),
                        QT_TRANSLATE_NOOP("Shortcuts", "Move the performance overlay"),
                        QT_TRANSLATE_NOOP("Shortcuts", "Window"),
                        {} },
        makeAction(ShortcutActions::UngrabInput,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle mouse and keyboard capture"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'Z'),
        makeAction(ShortcutActions::ToggleMouseMode,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle remote desktop mouse mode"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'M'),
        makeAction(ShortcutActions::ToggleCursorHide,
                   QT_TRANSLATE_NOOP("Shortcuts", "Show or hide the local cursor"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'C'),
        makeAction(ShortcutActions::TogglePointerRegionLock,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle pointer region lock"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'L'),
        makeAction(ShortcutActions::ToggleKeyboardGrab,
                   QT_TRANSLATE_NOOP("Shortcuts", "Toggle system keyboard shortcut capture"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'K'),
        makeAction(ShortcutActions::PasteText,
                   QT_TRANSLATE_NOOP("Shortcuts", "Type clipboard text on the host"),
                   QT_TRANSLATE_NOOP("Shortcuts", "Input"), 'V'),
        makeUnboundAction(ShortcutActions::SendCtrlAltDel,
                          QT_TRANSLATE_NOOP("Shortcuts", "Send Ctrl+Alt+Del to the host"),
                          QT_TRANSLATE_NOOP("Shortcuts", "Host")),
        makeUnboundAction(ShortcutActions::OpenHostCommands,
                          QT_TRANSLATE_NOOP("Shortcuts", "Open the host commands menu"),
                          QT_TRANSLATE_NOOP("Shortcuts", "Host")),
        makeHoldAction(ShortcutActions::RightAltMouseRelease,
                       QT_TRANSLATE_NOOP("Shortcuts", "Hold Right Alt to release the mouse"),
                       QT_TRANSLATE_NOOP("Shortcuts", "Input")),
    };
}

QVector<ShortcutBinding> emptyBindings()
{
    return QVector<ShortcutBinding>(ShortcutRegistry::MaxBindings);
}

QString settingsKey(const QString& actionId)
{
    return QStringLiteral(SER_BINDINGS "/") + actionId;
}

} // namespace

QString ShortcutKeys::toToken(int scancode)
{
    if (scancode <= Unknown || scancode >= MaxScancode) {
        return QString();
    }

    QString token = generatedToken(scancode);
    if (!token.isEmpty()) {
        return token;
    }

    const NamedKey* named = findNamedKey(scancode);
    if (named != nullptr) {
        return QLatin1String(named->token);
    }
    return QStringLiteral("sc%1").arg(scancode);
}

int ShortcutKeys::fromToken(const QString& token)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return Unknown;
    }

    for (int scancode = 1; scancode < MaxScancode; scancode++) {
        const QString generated = generatedToken(scancode);
        if (!generated.isEmpty() && generated.compare(trimmed, Qt::CaseInsensitive) == 0) {
            return scancode;
        }
    }

    for (const NamedKey& key : k_NamedKeys) {
        if (trimmed.compare(QLatin1String(key.token), Qt::CaseInsensitive) == 0) {
            return key.scancode;
        }
    }

    if (trimmed.startsWith(QLatin1String("sc"), Qt::CaseInsensitive)) {
        bool ok;
        const int scancode = trimmed.mid(2).toInt(&ok, 10);
        if (ok && scancode > Unknown && scancode < MaxScancode) {
            return scancode;
        }
    }
    return Unknown;
}

bool ShortcutKeys::isModifierKey(int scancode)
{
    return scancode >= LeftCtrl && scancode <= RightGui;
}

bool ShortcutKeys::isLatinKey(int scancode)
{
    return (scancode >= A && scancode <= Z) || (scancode >= Num1 && scancode <= Num0);
}

bool ShortcutKeys::isFunctionKey(int scancode)
{
    return (scancode >= F1 && scancode <= F12) || isExtendedFunctionKey(scancode);
}

bool ShortcutKeys::isExtendedFunctionKey(int scancode)
{
    return scancode >= F13 && scancode <= F24;
}

bool ShortcutKeys::isMediaKey(int scancode)
{
    switch (scancode) {
    case Mute:
    case VolumeUp:
    case VolumeDown:
    case AudioNext:
    case AudioPrev:
    case AudioStop:
    case AudioPlay:
    case AudioMute:
        return true;
    default:
        return false;
    }
}

int ShortcutKeys::fromQtKeyEvent(int qtKey, quint32 nativeScanCode)
{
    // Latin letters and digits are matched by character (see shortcuts.h)
    if ((qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) || (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)) {
        return fromQtKey(qtKey);
    }

    if (nativeScanCode != 0) {
        const int scancode = fromNativeScanCode(nativeScanCode);
        if (scancode != Unknown) {
            return scancode;
        }
    }
    return fromQtKey(qtKey);
}

bool ShortcutBinding::isValid() const
{
    if (scancode <= ShortcutKeys::Unknown || scancode >= ShortcutKeys::MaxScancode ||
        ShortcutKeys::isModifierKey(scancode) || (modifiers & ~ModAll) != 0) {
        return false;
    }

    if (modifiers & (ModCtrl | ModAlt | ModGui)) {
        return true;
    }

    // No modifiers (or Shift alone) would steal normal typing, so only keys
    // that games and apps rarely use may be bound this way.
    return ShortcutKeys::isExtendedFunctionKey(scancode) || ShortcutKeys::isMediaKey(scancode);
}

bool ShortcutBinding::isValidHold() const
{
    if (isValid()) {
        return true;
    }
    return ShortcutKeys::isModifierKey(scancode) && (modifiers & ~ModAll) == 0 &&
           (modifiers & modifierForKey(scancode)) == 0;
}

quint8 ShortcutBinding::modifierForKey(int scancode)
{
    switch (scancode) {
    case ShortcutKeys::LeftCtrl:
    case ShortcutKeys::RightCtrl:
        return ModCtrl;
    case ShortcutKeys::LeftShift:
    case ShortcutKeys::RightShift:
        return ModShift;
    case ShortcutKeys::LeftAlt:
    case ShortcutKeys::RightAlt:
        return ModAlt;
    case ShortcutKeys::LeftGui:
    case ShortcutKeys::RightGui:
        return ModGui;
    default:
        return ModNone;
    }
}

QString ShortcutBinding::toString() const
{
    if (isNull()) {
        return QString();
    }

    QString string;
    for (const auto& mod : k_ModifierTokens) {
        if (modifiers & mod.modifier) {
            string += QLatin1String(mod.token);
            string += QLatin1Char('+');
        }
    }
    return string + ShortcutKeys::toToken(scancode);
}

ShortcutBinding ShortcutBinding::fromString(const QString& string)
{
    const QStringList parts = string.trimmed().split(QLatin1Char('+'));
    if (parts.isEmpty()) {
        return ShortcutBinding();
    }

    quint8 modifiers = ModNone;
    for (int i = 0; i < parts.size() - 1; i++) {
        const quint8 modifier = parseModifierToken(parts[i].trimmed());
        if (modifier == ModNone) {
            return ShortcutBinding();
        }
        modifiers |= modifier;
    }

    // Modifier keys parse too; ShortcutAction::accepts() decides whether the
    // action allows them
    const int scancode = ShortcutKeys::fromToken(parts.last());
    if (scancode == ShortcutKeys::Unknown) {
        return ShortcutBinding();
    }
    return ShortcutBinding(modifiers, scancode);
}

QString ShortcutBinding::toDisplayString() const
{
    if (isNull()) {
        return QString();
    }

    QStringList parts;
    if (modifiers & ModCtrl) {
        parts.append(QStringLiteral("Ctrl"));
    }
    if (modifiers & ModAlt) {
        parts.append(QStringLiteral("Alt"));
    }
    if (modifiers & ModShift) {
        parts.append(QStringLiteral("Shift"));
    }
    if (modifiers & ModGui) {
#if defined(Q_OS_DARWIN)
        parts.append(QStringLiteral("Cmd"));
#elif defined(Q_OS_WIN32)
        parts.append(QStringLiteral("Win"));
#else
        parts.append(QStringLiteral("Super"));
#endif
    }

    QString key = generatedToken(scancode);
    if (key.startsWith(QLatin1String("Kp"))) {
        key = QStringLiteral("Num ") + key.mid(2);
    } else if (ShortcutKeys::isModifierKey(scancode)) {
        key = modifierKeyDisplay(scancode);
    } else if (key.isEmpty()) {
        const NamedKey* named = findNamedKey(scancode);
        key =
            named != nullptr ? QString::fromUtf8(named->display) : ShortcutKeys::toToken(scancode);
    }
    parts.append(key);

    return parts.join(QLatin1Char('+'));
}

quint8 ShortcutBinding::fromQtModifiers(int qtModifiers)
{
    quint8 modifiers = ModNone;
    int ctrlModifier = Qt::ControlModifier;
    int guiModifier = Qt::MetaModifier;

#ifdef Q_OS_DARWIN
    // Qt reports Command as ControlModifier on macOS unless told otherwise
    if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) {
        ctrlModifier = Qt::MetaModifier;
        guiModifier = Qt::ControlModifier;
    }
#endif

    if (qtModifiers & ctrlModifier) {
        modifiers |= ModCtrl;
    }
    if (qtModifiers & Qt::AltModifier) {
        modifiers |= ModAlt;
    }
    if (qtModifiers & Qt::ShiftModifier) {
        modifiers |= ModShift;
    }
    if (qtModifiers & guiModifier) {
        modifiers |= ModGui;
    }
    return modifiers;
}

QString ShortcutAction::translatedName() const
{
    return QCoreApplication::translate("Shortcuts", name);
}

QString ShortcutAction::translatedGroup() const
{
    return QCoreApplication::translate("Shortcuts", group);
}

const ShortcutRegistry& ShortcutRegistry::instance()
{
    static const ShortcutRegistry registry(builtinActions());
    return registry;
}

ShortcutRegistry::ShortcutRegistry(QVector<ShortcutAction> actions) : m_Actions(std::move(actions))
{
}

const ShortcutAction* ShortcutRegistry::find(const QString& id) const
{
    for (const ShortcutAction& action : m_Actions) {
        if (action.id == id) {
            return &action;
        }
    }
    return nullptr;
}

ShortcutProfile::ShortcutProfile(const ShortcutRegistry& registry)
    : m_Registry(&registry), m_Prefix(PrefixCtrlAltShift)
{
}

quint8 ShortcutProfile::prefixModifiers(DefaultPrefix prefix)
{
    switch (prefix) {
    case PrefixCtrlAlt:
        return ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt;
    case PrefixCtrlAltShift:
    default:
        return ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt | ShortcutBinding::ModShift;
    }
}

void ShortcutProfile::load(QSettings& settings)
{
    m_Overrides.clear();
    m_Prefix = PrefixCtrlAltShift;

    settings.beginGroup(QStringLiteral(SER_SHORTCUTS));

    const QVariant legacy =
        settings.value(settingsKey(QLatin1String(ShortcutActions::HoldReleaseMouse)));
    const QStringList legacyBindings =
        legacy.toStringList().join(QLatin1Char(',')).split(QLatin1Char(','));
    const bool legacyRightAlt =
        std::any_of(legacyBindings.cbegin(), legacyBindings.cend(), [](const QString& entry) {
            return ShortcutBinding::fromString(entry) ==
                   ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::RightAlt);
        });
    m_RightAltReleaseEnabled =
        settings.value(QStringLiteral(SER_RIGHT_ALT_RELEASE), legacyRightAlt).toBool();
    const QString prefix = settings.value(QStringLiteral(SER_DEFAULTPREFIX)).toString();
    if (prefix == QLatin1String(k_PrefixTokens[PrefixCtrlAlt])) {
        m_Prefix = PrefixCtrlAlt;
    }

    for (const ShortcutAction& action : m_Registry->actions()) {
        const QString key = settingsKey(action.id);
        if (!settings.contains(key)) {
            continue;
        }

        // INI files may hand a comma-separated value back as a string list
        const QVariant value = settings.value(key);
        const QString string = value.canConvert<QStringList>() && value.toStringList().size() > 1
                                   ? value.toStringList().join(QLatin1Char(','))
                                   : value.toString();

        QVector<ShortcutBinding> bindings = emptyBindings();
        const QStringList entries = string.split(QLatin1Char(','));
        for (int slot = 0; slot < entries.size() && slot < ShortcutRegistry::MaxBindings; slot++) {
            const ShortcutBinding binding = ShortcutBinding::fromString(entries[slot]);
            if (action.accepts(binding)) {
                bindings[slot] = binding;
            }
        }
        m_Overrides.insert(action.id, bindings);
    }

    settings.endGroup();
}

void ShortcutProfile::save(QSettings& settings) const
{
    settings.beginGroup(QStringLiteral(SER_SHORTCUTS));

    settings.setValue(QStringLiteral(SER_DEFAULTPREFIX), QLatin1String(k_PrefixTokens[m_Prefix]));
    settings.setValue(QStringLiteral(SER_RIGHT_ALT_RELEASE), m_RightAltReleaseEnabled);

    // Only touch keys of known actions, so a build with a different set of
    // actions doesn't wipe out the others' settings.
    for (const ShortcutAction& action : m_Registry->actions()) {
        const QString key = settingsKey(action.id);
        auto it = m_Overrides.constFind(action.id);
        if (it == m_Overrides.constEnd()) {
            settings.remove(key);
            continue;
        }

        QStringList entries;
        for (const ShortcutBinding& binding : *it) {
            entries.append(binding.toString());
        }
        while (!entries.isEmpty() && entries.last().isEmpty()) {
            entries.removeLast();
        }
        settings.setValue(key, entries.join(QLatin1Char(',')));
    }

    settings.endGroup();
}

ShortcutProfile ShortcutProfile::loadFromSettings()
{
    QSettings settings;
    ShortcutProfile profile;
    profile.load(settings);
    return profile;
}

void ShortcutProfile::saveToSettings() const
{
    QSettings settings;
    save(settings);
}

QVector<ShortcutBinding> ShortcutProfile::defaultBindings(const QString& actionId) const
{
    QVector<ShortcutBinding> bindings = emptyBindings();
    const ShortcutAction* action = m_Registry->find(actionId);
    if (action == nullptr) {
        return bindings;
    }

    for (int slot = 0; slot < action->defaults.size() && slot < ShortcutRegistry::MaxBindings;
         slot++) {
        const ShortcutAction::DefaultBinding& def = action->defaults[slot];
        bindings[slot] = ShortcutBinding(def.usePrefix ? def.modifiers | prefixModifiers(m_Prefix)
                                                       : def.modifiers,
                                         def.scancode);
    }
    return bindings;
}

QVector<ShortcutBinding> ShortcutProfile::bindings(const QString& actionId) const
{
    if (actionId == QLatin1String(ShortcutActions::RightAltMouseRelease)) {
        auto result = emptyBindings();
        if (m_RightAltReleaseEnabled && m_Registry->find(actionId) != nullptr) {
            result[0] = ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::RightAlt);
        }
        return result;
    }
    auto it = m_Overrides.constFind(actionId);
    if (it != m_Overrides.constEnd()) {
        return *it;
    }
    return defaultBindings(actionId);
}

bool ShortcutProfile::isCustomized(const QString& actionId) const
{
    return m_Overrides.contains(actionId);
}

bool ShortcutProfile::setBinding(const QString& actionId, int slot, const ShortcutBinding& binding)
{
    if (actionId == QLatin1String(ShortcutActions::RightAltMouseRelease)) {
        return false;
    }
    const ShortcutAction* action = m_Registry->find(actionId);
    if (action == nullptr || slot < 0 || slot >= ShortcutRegistry::MaxBindings ||
        (!binding.isNull() && !action->accepts(binding))) {
        return false;
    }

    QVector<ShortcutBinding> bindings = this->bindings(actionId);
    bindings[slot] = binding;

    // The same binding twice in one action is pointless; keep the new slot
    for (int other = 0; other < bindings.size(); other++) {
        if (other != slot && !binding.isNull() && bindings[other] == binding) {
            bindings[other] = ShortcutBinding();
        }
    }

    // Matching the defaults means following future prefix changes again
    if (bindings == defaultBindings(actionId)) {
        m_Overrides.remove(actionId);
    } else {
        m_Overrides.insert(actionId, bindings);
    }
    return true;
}

void ShortcutProfile::resetAction(const QString& actionId)
{
    m_Overrides.remove(actionId);
}

void ShortcutProfile::resetAll()
{
    m_Overrides.clear();
    m_Prefix = PrefixCtrlAltShift;
    m_RightAltReleaseEnabled = false;
}

QVector<ShortcutProfile::Conflict> ShortcutProfile::conflicts() const
{
    struct Slot
    {
        QString actionId;
        int slot;
        ShortcutBinding binding;
    };

    QVector<Slot> used;
    for (const ShortcutAction& action : m_Registry->actions()) {
        const QVector<ShortcutBinding> actionBindings = bindings(action.id);
        for (int slot = 0; slot < actionBindings.size(); slot++) {
            if (!actionBindings[slot].isNull()) {
                used.append({ action.id, slot, actionBindings[slot] });
            }
        }
    }

    QVector<Conflict> result;
    for (int i = 0; i < used.size(); i++) {
        for (int j = i + 1; j < used.size(); j++) {
            if (used[i].binding == used[j].binding) {
                result.append({ used[i].binding, used[i].actionId, used[i].slot, used[j].actionId,
                                used[j].slot });
            }
        }
    }
    return result;
}

QStringList ShortcutProfile::conflictingActions(const QString& actionId, int slot) const
{
    QStringList result;
    const QVector<ShortcutBinding> own = bindings(actionId);
    if (slot < 0 || slot >= own.size() || own[slot].isNull()) {
        return result;
    }

    for (const ShortcutAction& action : m_Registry->actions()) {
        if (action.id == actionId) {
            continue;
        }
        if (bindings(action.id).contains(own[slot])) {
            result.append(action.id);
        }
    }
    return result;
}

void ShortcutDispatcher::setHandler(const QString& actionId, Handler handler)
{
    if (handler) {
        m_Handlers.insert(actionId, std::move(handler));
    } else {
        m_Handlers.remove(actionId);
    }
}

void ShortcutDispatcher::setHoldHandler(const QString& actionId, HoldHandler handler)
{
    if (handler) {
        m_HoldHandlers.insert(actionId, std::move(handler));
    } else {
        m_HoldHandlers.remove(actionId);
    }
}

void ShortcutDispatcher::removeHandler(const QString& actionId)
{
    m_Handlers.remove(actionId);
    m_HoldHandlers.remove(actionId);
}

bool ShortcutDispatcher::hasHandler(const QString& actionId) const
{
    return m_Handlers.contains(actionId) || m_HoldHandlers.contains(actionId);
}

QStringList ShortcutDispatcher::actionIds() const
{
    QStringList ids = m_Handlers.keys();
    for (auto it = m_HoldHandlers.constBegin(); it != m_HoldHandlers.constEnd(); ++it) {
        if (!ids.contains(it.key())) {
            ids.append(it.key());
        }
    }
    return ids;
}

bool ShortcutDispatcher::trigger(const QString& actionId) const
{
    auto it = m_Handlers.constFind(actionId);
    if (it == m_Handlers.constEnd()) {
        return false;
    }

    // Copy, since the handler may replace itself
    const Handler handler = *it;
    handler();
    return true;
}

bool ShortcutDispatcher::triggerHold(const QString& actionId, bool held) const
{
    auto it = m_HoldHandlers.constFind(actionId);
    if (it == m_HoldHandlers.constEnd()) {
        return false;
    }

    const HoldHandler handler = *it;
    handler(held);
    return true;
}

void ModifierKeyRecorder::press(int scancode, quint8 modifiers)
{
    if (!ShortcutKeys::isModifierKey(scancode)) {
        reset();
        return;
    }

    quint8 others =
        modifiers & ShortcutBinding::ModAll & ~ShortcutBinding::modifierForKey(scancode);
    if (m_AltGrFakesControl && scancode == ShortcutKeys::RightAlt &&
        m_Pending == ShortcutKeys::LeftCtrl && m_PendingModifiers == ShortcutBinding::ModNone) {
        others &= ~ShortcutBinding::ModCtrl;
    }
    m_Pending = scancode;
    m_PendingModifiers = others;
}

ShortcutBinding ModifierKeyRecorder::release(int scancode)
{
    if (scancode == ShortcutKeys::Unknown || scancode != m_Pending) {
        // Another modifier coming up first (e.g. AltGr's fake Ctrl) keeps the
        // pending one
        return ShortcutBinding();
    }

    const ShortcutBinding binding(m_PendingModifiers, m_Pending);
    reset();
    return binding;
}

void ShortcutMatcher::setProfile(const ShortcutProfile& profile)
{
    m_ReserveRightAlt = profile.rightAltReleaseEnabled();
    m_Bindings.clear();
    for (const ShortcutAction& action : profile.registry().actions()) {
        for (const ShortcutBinding& binding : profile.bindings(action.id)) {
            if (action.accepts(binding)) {
                m_Bindings[binding.key()].append(action.id);
            }
        }
    }
}

QString ShortcutMatcher::actionFor(const ShortcutBinding& binding,
                                   const ShortcutDispatcher* dispatcher) const
{
    auto it = m_Bindings.constFind(binding.key());
    if (it == m_Bindings.constEnd()) {
        return QString();
    }

    for (const QString& actionId : *it) {
        if (dispatcher == nullptr || dispatcher->hasHandler(actionId)) {
            return actionId;
        }
    }
    return QString();
}

bool ShortcutMatcher::processKeyEvent(int scancode, int latinKey, quint8 modifiers, bool pressed,
                                      bool repeat, const ShortcutDispatcher& dispatcher)
{
    if (!pressed) {
        // A hold ends with the key-up of the key that started it, whatever
        // modifiers are down by then
        const auto held = m_HeldActions.constFind(scancode);
        if (held != m_HeldActions.constEnd()) {
            const QString actionId = *held;
            m_HeldActions.erase(held);
            m_SwallowedKeys.remove(scancode);
            dispatcher.triggerHold(actionId, false);
            return true;
        }

        // Swallow the key-up that belongs to a swallowed key-down
        const bool swallowed = m_SwallowedKeys.remove(scancode);
        return swallowed ||
               (m_ReserveRightAlt && scancode == ShortcutKeys::RightAlt &&
                dispatcher.hasHandler(QLatin1String(ShortcutActions::RightAltMouseRelease)));
    }

    if (repeat) {
        return m_SwallowedKeys.contains(scancode);
    }

    // A fresh press. If the key was still marked, we lost its key-up (e.g.
    // focus moved to another window), so start over.
    m_SwallowedKeys.remove(scancode);
    m_HeldActions.remove(scancode);

    // Ignore the key's own modifier, and those of swallowed modifier keys,
    // which the host and the bindings never see
    const quint8 exactModifiers = m_ReserveRightAlt && scancode == ShortcutKeys::RightAlt
                                      ? 0
                                      : modifiers & ShortcutBinding::ModAll &
                                            ~ShortcutBinding::modifierForKey(scancode) &
                                            ~suppressedModifiers();

    // Character pass over all bindings, then the physical key pass
    QString actionId;
    if (ShortcutKeys::isLatinKey(latinKey)) {
        actionId = actionFor(ShortcutBinding(exactModifiers, latinKey), &dispatcher);
    }
    if (actionId.isEmpty()) {
        actionId = actionFor(ShortcutBinding(exactModifiers, scancode), &dispatcher);
    }
    if (actionId.isEmpty()) {
        return false;
    }

    // Mark first, since the handler may reset us
    m_SwallowedKeys.insert(scancode);
    if (dispatcher.triggerHold(actionId, true)) {
        m_HeldActions.insert(scancode, actionId);
    } else {
        dispatcher.trigger(actionId);
    }
    return true;
}

bool ShortcutMatcher::processAltGrControl(bool pressed, const ShortcutDispatcher& dispatcher)
{
    if (!pressed) {
        const bool swallowed = m_AltGrControlSwallowed;
        m_AltGrControlSwallowed = false;
        return swallowed;
    }

    // Only worth hiding when the Right Alt that follows will be swallowed too;
    // otherwise the host needs Ctrl+Alt to type AltGr characters
    m_AltGrControlSwallowed =
        !actionFor(ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::RightAlt), &dispatcher)
             .isEmpty();
    return m_AltGrControlSwallowed;
}

quint8 ShortcutMatcher::suppressedModifiers() const
{
    quint8 modifiers =
        m_AltGrControlSwallowed ? ShortcutBinding::ModCtrl : ShortcutBinding::ModNone;
    for (auto it = m_HeldActions.constBegin(); it != m_HeldActions.constEnd(); ++it) {
        modifiers |= ShortcutBinding::modifierForKey(it.key());
    }
    return modifiers;
}
