#include "hostkeys.h"

#include <QCoreApplication>
#include <QSettings>

#define SER_KEYMACROS "keyMacros"
#define SER_MACROS "macros"
#define SER_NAME "name"
#define SER_KEYS "keys"

namespace {

// SDL2 scancodes (USB HID usage IDs) of the modifier keys
enum : int
{
    LeftCtrl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    LeftGui = 227,
    RightCtrl = 228,
    RightShift = 229,
    RightAlt = 230,
    RightGui = 231,
};

struct VirtualKeyEntry
{
    int scancode;
    quint16 vk;
    bool extended;
};

// Mirrors the switch in SdlInputHandler::handleKeyEvent(). Keys that need
// SS_KBE_FLAG_NON_NORMALIZED there (International1/3) are left out.
const VirtualKeyEntry k_VirtualKeys[] = {
    { 40, 0x0D, false },  // Return
    { 41, 0x1B, false },  // Escape
    { 42, 0x08, false },  // Backspace
    { 43, 0x09, false },  // Tab
    { 44, 0x20, false },  // Space
    { 45, 0xBD, false },  // Minus
    { 46, 0xBB, false },  // Equals
    { 47, 0xDB, false },  // Left bracket
    { 48, 0xDD, false },  // Right bracket
    { 49, 0xDC, false },  // Backslash
    { 51, 0xBA, false },  // Semicolon
    { 52, 0xDE, false },  // Apostrophe
    { 53, 0xC0, false },  // Grave
    { 54, 0xBC, false },  // Comma
    { 55, 0xBE, false },  // Period
    { 56, 0xBF, false },  // Slash
    { 57, 0x14, false },  // Caps Lock
    { 70, 0x2C, true },   // Print Screen
    { 71, 0x91, false },  // Scroll Lock
    { 72, 0x13, false },  // Pause
    { 73, 0x2D, true },   // Insert
    { 74, 0x24, true },   // Home
    { 75, 0x21, true },   // Page Up
    { 76, 0x2E, true },   // Delete
    { 77, 0x23, true },   // End
    { 78, 0x22, true },   // Page Down
    { 79, 0x27, true },   // Right
    { 80, 0x25, true },   // Left
    { 81, 0x28, true },   // Down
    { 82, 0x26, true },   // Up
    { 83, 0x90, false },  // Num Lock
    { 84, 0x6F, true },   // Keypad /
    { 85, 0x6A, false },  // Keypad *
    { 86, 0x6D, false },  // Keypad -
    { 87, 0x6B, false },  // Keypad +
    { 88, 0x0D, true },   // Keypad Enter
    { 98, 0x60, false },  // Keypad 0
    { 99, 0x6E, false },  // Keypad .
    { 100, 0xE2, false }, // Non-US backslash
    { 101, 0x5D, true },  // Application (Menu)
    { 116, 0x2B, false }, // Execute
    { 117, 0x2F, false }, // Help
    { 119, 0x29, false }, // Select
    { 133, 0x6C, false }, // Keypad comma
    { 144, 0x1C, false }, // Lang1
    { 145, 0x1D, false }, // Lang2
    { 156, 0x0C, false }, // Clear
    { LeftCtrl, 0xA2, false }, { LeftShift, 0xA0, false }, { LeftAlt, 0xA4, false },
    { LeftGui, 0x5B, true },   { RightCtrl, 0xA3, true },  { RightShift, 0xA1, false },
    { RightAlt, 0xA5, true },  { RightGui, 0x5C, true },   { 268, 0xAA, true }, // AC Search
    { 269, 0xAC, true },                                                        // AC Home
    { 270, 0xA6, true },                                                        // AC Back
    { 271, 0xA7, true },                                                        // AC Forward
    { 272, 0xA9, true },                                                        // AC Stop
    { 273, 0xA8, true },                                                        // AC Refresh
    { 274, 0xAB, true },                                                        // AC Bookmarks
};

struct ModifierKey
{
    quint8 bindingModifier;
    int scancode;
    quint8 hostModifier;
    const char* display;
};

// Press order; released in reverse
const ModifierKey k_ModifierKeys[] = {
    { ShortcutBinding::ModCtrl, LeftCtrl, HostKeys::HostCtrl, "Ctrl" },
    { ShortcutBinding::ModAlt, LeftAlt, HostKeys::HostAlt, "Alt" },
    { ShortcutBinding::ModShift, LeftShift, HostKeys::HostShift, "Shift" },
    { ShortcutBinding::ModGui, LeftGui, HostKeys::HostMeta, "Win" },
};

const ModifierKey* modifierKeyFor(int scancode)
{
    switch (scancode) {
    case LeftCtrl:
    case RightCtrl:
        return &k_ModifierKeys[0];
    case LeftAlt:
    case RightAlt:
        return &k_ModifierKeys[1];
    case LeftShift:
    case RightShift:
        return &k_ModifierKeys[2];
    case LeftGui:
    case RightGui:
        return &k_ModifierKeys[3];
    default:
        return nullptr;
    }
}

HostKeys::KeyStroke makeStroke(int scancode, bool down, quint8 held)
{
    bool extended = false;
    const quint16 vk = HostKeys::virtualKey(scancode, &extended);
    return HostKeys::KeyStroke{ vk, down, quint8(held | (extended ? HostKeys::HostExtended : 0)) };
}

const char k_SpecialPrefix[] = "special:";
const char k_KeysPrefix[] = "keys:";

} // namespace

quint16 HostKeys::virtualKey(int scancode, bool* extended)
{
    if (extended != nullptr) {
        *extended = false;
    }

    if (scancode >= ShortcutKeys::A && scancode <= ShortcutKeys::Z) {
        return quint16(0x41 + (scancode - ShortcutKeys::A));
    } else if (scancode >= ShortcutKeys::Num1 && scancode < ShortcutKeys::Num0) {
        return quint16(0x31 + (scancode - ShortcutKeys::Num1));
    } else if (scancode == ShortcutKeys::Num0) {
        return 0x30;
    } else if (scancode >= ShortcutKeys::F1 && scancode <= ShortcutKeys::F12) {
        return quint16(0x70 + (scancode - ShortcutKeys::F1));
    } else if (scancode >= ShortcutKeys::F13 && scancode <= ShortcutKeys::F24) {
        return quint16(0x7C + (scancode - ShortcutKeys::F13));
    } else if (scancode >= 89 && scancode <= 97) {
        // Keypad 1-9
        return quint16(0x61 + (scancode - 89));
    }

    for (const VirtualKeyEntry& entry : k_VirtualKeys) {
        if (entry.scancode == scancode) {
            if (extended != nullptr) {
                *extended = entry.extended;
            }
            return entry.vk;
        }
    }
    return 0;
}

bool HostKeys::canSend(const ShortcutBinding& binding)
{
    return !binding.isNull() && (binding.modifiers & ~ShortcutBinding::ModAll) == 0 &&
           virtualKey(binding.scancode) != 0;
}

QVector<HostKeys::KeyStroke> HostKeys::strokes(const ShortcutBinding& binding)
{
    QVector<KeyStroke> result;
    if (!canSend(binding)) {
        return result;
    }

    quint8 held = 0;
    for (const ModifierKey& modifier : k_ModifierKeys) {
        if (binding.modifiers & modifier.bindingModifier) {
            held |= modifier.hostModifier;
            result.append(makeStroke(modifier.scancode, true, held));
        }
    }

    // A lone modifier key (e.g. Win) carries its own bit while it is down
    const ModifierKey* own = modifierKeyFor(binding.scancode);
    result.append(makeStroke(binding.scancode, true,
                             quint8(held | (own != nullptr ? own->hostModifier : 0))));
    result.append(makeStroke(binding.scancode, false, held));

    for (int i = int(sizeof(k_ModifierKeys) / sizeof(k_ModifierKeys[0])) - 1; i >= 0; i--) {
        const ModifierKey& modifier = k_ModifierKeys[i];
        if (binding.modifiers & modifier.bindingModifier) {
            held &= quint8(~modifier.hostModifier);
            result.append(makeStroke(modifier.scancode, false, held));
        }
    }
    return result;
}

QString HostKeys::displayText(const ShortcutBinding& binding)
{
    if (binding.isNull()) {
        return QString();
    }

    QStringList parts;
    for (const ModifierKey& modifier : k_ModifierKeys) {
        if (binding.modifiers & modifier.bindingModifier) {
            parts.append(QLatin1String(modifier.display));
        }
    }

    const ModifierKey* own = modifierKeyFor(binding.scancode);
    parts.append(
        own != nullptr
            ? QString::fromLatin1(own->display)
            : ShortcutBinding(ShortcutBinding::ModNone, binding.scancode).toDisplayString());
    return parts.join(QLatin1Char('+'));
}

QString HostKeys::SpecialKey::translatedName() const
{
    return QCoreApplication::translate("HostKeys", name);
}

const QVector<HostKeys::SpecialKey>& HostKeys::specialKeys()
{
    static const QVector<SpecialKey> keys = {
        { "win", QT_TRANSLATE_NOOP("HostKeys", "Start menu"),
          ShortcutBinding(ShortcutBinding::ModNone, LeftGui) },
        { "alt-tab", QT_TRANSLATE_NOOP("HostKeys", "Switch windows"),
          ShortcutBinding(ShortcutBinding::ModAlt, 43) },
        { CtrlAltDel, QT_TRANSLATE_NOOP("HostKeys", "Security options"),
          ShortcutBinding(ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt, 76) },
        { "ctrl-shift-esc", QT_TRANSLATE_NOOP("HostKeys", "Task Manager"),
          ShortcutBinding(ShortcutBinding::ModCtrl | ShortcutBinding::ModShift,
                          ShortcutKeys::Escape) },
        { "win-g", QT_TRANSLATE_NOOP("HostKeys", "Game Bar"),
          ShortcutBinding(ShortcutBinding::ModGui, ShortcutKeys::letter('G')) },
        { "print-screen", QT_TRANSLATE_NOOP("HostKeys", "Screenshot"),
          ShortcutBinding(ShortcutBinding::ModNone, 70) },
        { "win-ctrl-o", QT_TRANSLATE_NOOP("HostKeys", "On-screen keyboard"),
          ShortcutBinding(ShortcutBinding::ModCtrl | ShortcutBinding::ModGui,
                          ShortcutKeys::letter('O')) },
    };
    return keys;
}

const HostKeys::SpecialKey* HostKeys::findSpecialKey(const QString& id)
{
    for (const SpecialKey& key : specialKeys()) {
        if (id == QLatin1String(key.id)) {
            return &key;
        }
    }
    return nullptr;
}

QString HostKeys::normalizedName(const QString& name)
{
    return name.simplified().left(MaxNameLength).trimmed();
}

bool HostKeys::isValidMacro(const Macro& macro)
{
    return !normalizedName(macro.name).isEmpty() &&
           !ShortcutKeys::isModifierKey(macro.binding.scancode) && canSend(macro.binding);
}

QVector<HostKeys::Macro> HostKeys::loadMacros(QSettings& settings)
{
    QVector<Macro> macros;

    settings.beginGroup(QStringLiteral(SER_KEYMACROS));
    const int count = settings.beginReadArray(QStringLiteral(SER_MACROS));
    for (int i = 0; i < count && macros.size() < MaxMacros; i++) {
        settings.setArrayIndex(i);
        const Macro macro{ normalizedName(settings.value(QStringLiteral(SER_NAME)).toString()),
                           ShortcutBinding::fromString(
                               settings.value(QStringLiteral(SER_KEYS)).toString()) };
        if (isValidMacro(macro)) {
            macros.append(macro);
        }
    }
    settings.endArray();
    settings.endGroup();

    return macros;
}

void HostKeys::saveMacros(QSettings& settings, const QVector<Macro>& macros)
{
    settings.beginGroup(QStringLiteral(SER_KEYMACROS));
    // Drop entries left over from a longer list
    settings.remove(QString());

    settings.beginWriteArray(QStringLiteral(SER_MACROS));
    int index = 0;
    for (const Macro& macro : macros) {
        if (index >= MaxMacros || !isValidMacro(macro)) {
            continue;
        }
        settings.setArrayIndex(index++);
        settings.setValue(QStringLiteral(SER_NAME), normalizedName(macro.name));
        settings.setValue(QStringLiteral(SER_KEYS), macro.binding.toString());
    }
    settings.endArray();
    settings.endGroup();
}

QVector<HostKeys::Macro> HostKeys::loadMacrosFromSettings()
{
    QSettings settings;
    return loadMacros(settings);
}

void HostKeys::saveMacrosToSettings(const QVector<Macro>& macros)
{
    QSettings settings;
    saveMacros(settings, macros);
}

QString HostKeys::specialKeyPayload(const SpecialKey& key)
{
    return QLatin1String(k_SpecialPrefix) + QLatin1String(key.id);
}

QString HostKeys::macroPayload(const Macro& macro)
{
    return QLatin1String(k_KeysPrefix) + macro.binding.toString();
}

ShortcutBinding HostKeys::bindingForPayload(const QString& payload)
{
    if (payload.startsWith(QLatin1String(k_SpecialPrefix))) {
        const SpecialKey* key = findSpecialKey(payload.mid(int(sizeof(k_SpecialPrefix)) - 1));
        return key != nullptr ? key->binding : ShortcutBinding();
    } else if (payload.startsWith(QLatin1String(k_KeysPrefix))) {
        const ShortcutBinding binding =
            ShortcutBinding::fromString(payload.mid(int(sizeof(k_KeysPrefix)) - 1));
        return canSend(binding) ? binding : ShortcutBinding();
    }
    return ShortcutBinding();
}
