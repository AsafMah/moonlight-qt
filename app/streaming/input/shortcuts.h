#pragma once

// Customizable keyboard shortcuts.
//
// This file deliberately depends only on QtCore (no SDL, no QtGui) so it can be
// unit tested on its own. Keys are stored as SDL2 scancode values, which are USB
// HID usage IDs for the regular keyboard block. keyboard.cpp static_asserts that
// the values below match SDL's.
//
// Letter and digit keys follow the character on the key, like the original
// Ctrl+Alt+Shift combos: a binding on Q fires for the key that types "q" in the
// current layout (on AZERTY, the physical A key). Only when no binding matches
// that way, e.g. on non-latin layouts, is the physical key position used.

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QSettings;

namespace ShortcutKeys {
// A subset of SDL2 scancode values used by the default bindings and validation
enum : int {
    Unknown = 0,
    A = 4,
    Z = 29,
    Num1 = 30,
    Num0 = 39,
    Return = 40,
    Escape = 41,
    Backspace = 42,
    F1 = 58,
    F12 = 69,
    F13 = 104,
    F24 = 115,
    Mute = 127,
    VolumeUp = 128,
    VolumeDown = 129,
    LeftCtrl = 224,
    RightGui = 231,
    AudioNext = 258,
    AudioPrev = 259,
    AudioStop = 260,
    AudioPlay = 261,
    AudioMute = 262,
    MaxScancode = 512,
};

// Scancode of a letter key ('A'..'Z') in the US layout position
constexpr int letter(char c)
{
    return A + (c - 'A');
}

// Stable token for persisting a scancode ("Q", "F13", "PageUp", "sc200")
QString toToken(int scancode);

// Inverse of toToken(); case-insensitive. Returns Unknown for bad input.
int fromToken(const QString& token);

bool isModifierKey(int scancode);
bool isLatinKey(int scancode); // A-Z and 0-9, matched by character first
bool isFunctionKey(int scancode); // F1-F24
bool isExtendedFunctionKey(int scancode); // F13-F24
bool isMediaKey(int scancode);

// Converts a Qt key event (from QML or QKeyEvent) into a scancode, the same
// way the matcher sees SDL key events: a latin letter or digit maps to the US
// position of that character. Other keys use the native scan code where its
// meaning is known (Windows, X11/Wayland, macOS), and fall back to mapping the
// Qt::Key to its US layout position.
int fromQtKeyEvent(int qtKey, quint32 nativeScanCode);
}

struct ShortcutBinding
{
    enum Modifier : quint8 {
        ModNone = 0x0,
        ModCtrl = 0x1,
        ModAlt = 0x2,
        ModShift = 0x4,
        ModGui = 0x8,
        ModAll = ModCtrl | ModAlt | ModShift | ModGui,
    };

    quint8 modifiers = ModNone;
    int scancode = ShortcutKeys::Unknown;

    ShortcutBinding() = default;
    ShortcutBinding(quint8 modifiers, int scancode) : modifiers(modifiers), scancode(scancode) {}

    bool isNull() const { return scancode == ShortcutKeys::Unknown; }

    // Packs the binding into one integer, usable as a hash key
    int key() const { return (int(modifiers) << 16) | scancode; }

    // A binding the matcher will accept: a non-modifier key that has at least
    // one of Ctrl, Alt or Gui held. Shift alone does not count, since it would
    // steal ordinary typing. F13-F24 and media keys may be bound bare.
    bool isValid() const;

    // Persistent form, e.g. "ctrl+alt+shift+S". Empty for a null binding.
    QString toString() const;

    // Parses toString() output. Returns a null binding on malformed input.
    static ShortcutBinding fromString(const QString& string);

    // Human readable form, e.g. "Ctrl+Alt+Shift+S"
    QString toDisplayString() const;

    // Converts Qt::KeyboardModifiers bits into a modifier mask
    static quint8 fromQtModifiers(int qtModifiers);

    bool operator==(const ShortcutBinding& other) const
    {
        return modifiers == other.modifiers && scancode == other.scancode;
    }
    bool operator!=(const ShortcutBinding& other) const { return !(*this == other); }
};

// Stable action IDs. These are persisted, so never rename one.
namespace ShortcutActions {
constexpr const char Quit[] = "quit";
constexpr const char QuitAndExit[] = "quit-and-exit";
constexpr const char UngrabInput[] = "ungrab-input";
constexpr const char ToggleFullScreen[] = "toggle-fullscreen";
constexpr const char ToggleStatsOverlay[] = "toggle-stats-overlay";
constexpr const char ToggleMouseMode[] = "toggle-mouse-mode";
constexpr const char ToggleCursorHide[] = "toggle-cursor";
constexpr const char Minimize[] = "minimize";
constexpr const char PasteText[] = "paste-text";
constexpr const char TogglePointerRegionLock[] = "toggle-pointer-region-lock";
constexpr const char ToggleKeyboardGrab[] = "toggle-keyboard-grab";
// Moonlight V+ only
constexpr const char ToggleOverlayMenu[] = "toggle-overlay-menu";
}

struct ShortcutAction
{
    struct DefaultBinding
    {
        // When true, the user-selectable default prefix is added to modifiers
        bool usePrefix;
        quint8 modifiers;
        int scancode;
    };

    QString id;
    // Untranslated strings in the "Shortcuts" translation context
    const char* name;
    const char* group;
    QVector<DefaultBinding> defaults;

    QString translatedName() const;
    QString translatedGroup() const;
};

// The list of known actions, in display order
class ShortcutRegistry
{
public:
    static const ShortcutRegistry& instance();

    const QVector<ShortcutAction>& actions() const { return m_Actions; }
    const ShortcutAction* find(const QString& id) const;

    // Maximum number of bindings per action
    static constexpr int MaxBindings = 2;

    // Exposed for tests; instance() is the builtin list
    explicit ShortcutRegistry(QVector<ShortcutAction> actions);

private:
    QVector<ShortcutAction> m_Actions;
};

// The user's binding configuration: a default prefix plus per-action overrides
class ShortcutProfile
{
public:
    enum DefaultPrefix {
        PrefixCtrlAltShift,
        PrefixCtrlAlt,
    };

    struct Conflict
    {
        ShortcutBinding binding;
        QString firstAction;
        int firstSlot;
        QString secondAction;
        int secondSlot;
    };

    explicit ShortcutProfile(const ShortcutRegistry& registry = ShortcutRegistry::instance());

    // Loads from / saves to the "shortcuts" group of the given settings
    void load(QSettings& settings);
    void save(QSettings& settings) const;

    // Convenience wrappers using the application's default QSettings
    static ShortcutProfile loadFromSettings();
    void saveToSettings() const;

    const ShortcutRegistry& registry() const { return *m_Registry; }

    DefaultPrefix defaultPrefix() const { return m_Prefix; }
    void setDefaultPrefix(DefaultPrefix prefix) { m_Prefix = prefix; }
    static quint8 prefixModifiers(DefaultPrefix prefix);

    // Effective bindings for an action, always MaxBindings long. A null entry is
    // an empty slot. Unknown actions return all null entries.
    QVector<ShortcutBinding> bindings(const QString& actionId) const;
    QVector<ShortcutBinding> defaultBindings(const QString& actionId) const;

    bool isCustomized(const QString& actionId) const;

    // Sets one slot. A null binding clears it (disabling the action if both
    // slots are empty). Returns false for an unknown action or slot, or for a
    // binding that is neither null nor valid.
    bool setBinding(const QString& actionId, int slot, const ShortcutBinding& binding);

    void resetAction(const QString& actionId);
    void resetAll();

    // All pairs of slots that share the same binding
    QVector<Conflict> conflicts() const;

    // Actions other than (actionId, slot) that use the binding in that slot
    QStringList conflictingActions(const QString& actionId, int slot) const;

private:
    const ShortcutRegistry* m_Registry;
    DefaultPrefix m_Prefix;
    // Present only for customized actions; always MaxBindings long
    QHash<QString, QVector<ShortcutBinding>> m_Overrides;
};

// Maps action IDs to handlers. Shared by the keyboard matcher and any other
// UI that triggers actions by ID.
class ShortcutDispatcher
{
public:
    using Handler = std::function<void()>;

    void setHandler(const QString& actionId, Handler handler);
    void removeHandler(const QString& actionId);
    bool hasHandler(const QString& actionId) const;

    // Runs the handler. Returns false if the action has no handler.
    bool trigger(const QString& actionId) const;

    QStringList actionIds() const { return m_Handlers.keys(); }

private:
    QHash<QString, Handler> m_Handlers;
};

// Matches raw key events against a profile. Runs before normal key handling.
class ShortcutMatcher
{
public:
    // Takes a snapshot of the profile's effective bindings
    void setProfile(const ShortcutProfile& profile);

    // Action bound to this exact binding that has a handler in the dispatcher
    // (or any action if dispatcher is null). Empty when nothing matches.
    QString actionFor(const ShortcutBinding& binding,
                      const ShortcutDispatcher* dispatcher = nullptr) const;

    // Feeds one key event. latinKey is the US layout scancode of the character
    // the key types in the current layout (its keysym) when that is a latin
    // letter or digit, or Unknown otherwise.
    //
    // Matching runs in two passes, as the original combo code did: first every
    // binding is compared with latinKey, then, only if none matched, with the
    // physical scancode. Doing all character matches first keeps a binding
    // that matches by character from being shadowed by another binding whose
    // physical key happens to be the one pressed.
    //
    // Returns true when the event belongs to a shortcut and must not be
    // forwarded: the key-down that triggered it, its repeats, and the matching
    // key-up. Triggers the action on the initial key-down.
    bool processKeyEvent(int scancode, int latinKey, quint8 modifiers, bool pressed, bool repeat,
                         const ShortcutDispatcher& dispatcher);

    // Forgets keys being swallowed (e.g. after losing keyboard focus)
    void reset() { m_SwallowedKeys.clear(); }

private:
    // Binding key() to actions in registry order. Conflicting bindings list
    // several actions; the first one that has a handler wins.
    QHash<int, QStringList> m_Bindings;
    QSet<int> m_SwallowedKeys;
};
