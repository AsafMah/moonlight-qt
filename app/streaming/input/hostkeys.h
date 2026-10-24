#pragma once

// Key combinations sent to the host on request (overlay menu "Send keys",
// shortcut actions): a fixed set of special keys plus up to MaxMacros named
// macros that the user defines in the settings.
//
// Like shortcuts.h this depends only on QtCore so it can be unit tested on its
// own. Combos are ShortcutBinding values (SDL2 scancodes plus a modifier mask);
// strokes() turns one into the key-down/key-up sequence that keyboard.cpp would
// send for the same physical keys, and hostkeysender.cpp sends it.

#include "shortcuts.h"

#include <QString>
#include <QVector>

class QSettings;

namespace HostKeys {
// moonlight-common-c MODIFIER_* bits; hostkeysender.cpp static_asserts them
enum : quint8
{
    HostShift = 0x01,
    HostCtrl = 0x02,
    HostAlt = 0x04,
    HostMeta = 0x08,
    HostExtended = 0x10,
};

struct KeyStroke
{
    // Windows virtual-key code, as keyboard.cpp sends it (before | 0x8000)
    quint16 keyCode;
    bool down;
    // HostShift..HostMeta bits held at this point, plus HostExtended for
    // extended keys (0xE0 scancode prefix)
    quint8 modifiers;

    bool operator==(const KeyStroke& other) const
    {
        return keyCode == other.keyCode && down == other.down && modifiers == other.modifiers;
    }
    bool operator!=(const KeyStroke& other) const { return !(*this == other); }
};

// Windows virtual-key code of a scancode, using the same table as
// SdlInputHandler::handleKeyEvent() (left and right modifier keys included,
// without the Win/Alt swap). 0 for keys that keyboard.cpp does not send either.
quint16 virtualKey(int scancode, bool* extended = nullptr);

// True when the key has a virtual-key code and only known modifiers are set
bool canSend(const ShortcutBinding& binding);

// Key-down/key-up sequence for a combo. Ctrl, Alt, Shift and Win go down in
// that order, then the key goes down and up, then the modifiers go up in
// reverse. As in SDL key events, a modifier's key-down already carries its own
// bit and its key-up no longer does. Empty when !canSend(binding).
QVector<KeyStroke> strokes(const ShortcutBinding& binding);

// Sends strokes() of a combo with LiSendKeyboardEvent2(), the same call
// keyboard.cpp uses. Defined in hostkeysender.cpp so that this header and
// hostkeys.cpp stay free of moonlight-common-c. Returns false if the combo
// cannot be sent or a send failed (the remaining strokes are still sent so
// that no key is left down).
bool send(const ShortcutBinding& binding);

// Host-side text for a combo, e.g. "Ctrl+Alt+Delete". Unlike
// ShortcutBinding::toDisplayString() the Gui modifier is always "Win", since
// that is what the host receives, and a lone modifier key is named too.
QString displayText(const ShortcutBinding& binding);

struct SpecialKey
{
    const char* id;
    // Untranslated, in the "HostKeys" translation context
    const char* name;
    ShortcutBinding binding;

    QString translatedName() const;
};

// Win, Alt+Tab, Ctrl+Alt+Del, Ctrl+Shift+Esc, Win+G, Print Screen and
// Win+Ctrl+O (on-screen keyboard), in menu order
const QVector<SpecialKey>& specialKeys();
const SpecialKey* findSpecialKey(const QString& id);

// Special key IDs that are persisted or used by shortcut actions
constexpr const char CtrlAltDel[] = "ctrl-alt-del";

struct Macro
{
    QString name;
    ShortcutBinding binding;

    bool operator==(const Macro& other) const
    {
        return name == other.name && binding == other.binding;
    }
    bool operator!=(const Macro& other) const { return !(*this == other); }
};

constexpr int MaxMacros = 12;
constexpr int MaxNameLength = 40;

// Trimmed, with line breaks folded to spaces and cut to MaxNameLength
QString normalizedName(const QString& name);

// A storable macro: a non-empty normalized name and a sendable key that is
// not itself a modifier. Any set of modifiers, including none, is allowed.
bool isValidMacro(const Macro& macro);

// Loads from / saves to the "keyMacros" group. Loading skips invalid entries
// and stops at MaxMacros; saving replaces the whole group.
QVector<Macro> loadMacros(QSettings& settings);
void saveMacros(QSettings& settings, const QVector<Macro>& macros);

// Convenience wrappers using the application's default QSettings
QVector<Macro> loadMacrosFromSettings();
void saveMacrosToSettings(const QVector<Macro>& macros);

// Opaque overlay menu payloads ("special:<id>", "keys:<binding>") and their
// inverse. bindingForPayload() returns a null binding for unknown payloads.
QString specialKeyPayload(const SpecialKey& key);
QString macroPayload(const Macro& macro);
ShortcutBinding bindingForPayload(const QString& payload);
}
