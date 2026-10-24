// Host key combos: special keys, key-down/key-up sequences, and the custom
// key macro storage. See app/streaming/input/hostkeys.h.
#include "streaming/input/hostkeys.h"

#include <QCoreApplication>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
using HostKeys::KeyStroke;

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

const quint8 k_CtrlAlt = ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt;

KeyStroke down(quint16 vk, quint8 modifiers)
{
    return KeyStroke{ vk, true, modifiers };
}

KeyStroke up(quint16 vk, quint8 modifiers)
{
    return KeyStroke{ vk, false, modifiers };
}

ShortcutBinding special(const char* id)
{
    const HostKeys::SpecialKey* key = HostKeys::findSpecialKey(QLatin1String(id));
    return key != nullptr ? key->binding : ShortcutBinding();
}

bool testVirtualKeys(QTextStream& err)
{
    bool ok = true;
    const struct
    {
        int scancode;
        quint16 vk;
        bool extended;
    } expected[] = {
        { ShortcutKeys::letter('A'), 0x41, false },
        { ShortcutKeys::letter('Z'), 0x5A, false },
        { ShortcutKeys::Num1, 0x31, false },
        { ShortcutKeys::Num0, 0x30, false },
        { ShortcutKeys::F1, 0x70, false },
        { ShortcutKeys::F12, 0x7B, false },
        { ShortcutKeys::F13, 0x7C, false },
        { ShortcutKeys::F24, 0x87, false },
        { 89, 0x61, false },  // Keypad 1
        { 98, 0x60, false },  // Keypad 0
        { 88, 0x0D, true },   // Keypad Enter
        { 40, 0x0D, false },  // Enter
        { 76, 0x2E, true },   // Delete
        { 70, 0x2C, true },   // Print Screen
        { 227, 0x5B, true },  // Left Gui
        { 228, 0xA3, true },  // Right Ctrl
        { 226, 0xA4, false }, // Left Alt
    };
    for (const auto& key : expected) {
        bool extended = !key.extended;
        ok &= require(HostKeys::virtualKey(key.scancode, &extended) == key.vk &&
                          extended == key.extended,
                      QStringLiteral("scancode %1 maps wrong").arg(key.scancode), err);
    }

    // keyboard.cpp does not forward media keys either
    ok &= require(HostKeys::virtualKey(ShortcutKeys::Mute) == 0 &&
                      HostKeys::virtualKey(ShortcutKeys::AudioPlay) == 0 &&
                      HostKeys::virtualKey(ShortcutKeys::Unknown) == 0,
                  QStringLiteral("unsendable keys got a virtual-key code"), err);
    ok &= require(!HostKeys::canSend(ShortcutBinding(0, ShortcutKeys::VolumeUp)) &&
                      !HostKeys::canSend(ShortcutBinding()) &&
                      !HostKeys::canSend(ShortcutBinding(0x80, ShortcutKeys::letter('A'))),
                  QStringLiteral("unsendable combo accepted"), err);
    return ok;
}

bool testStrokes(QTextStream& err)
{
    bool ok = true;
    using namespace HostKeys;

    ok &= require(strokes(special(CtrlAltDel)) ==
                      QVector<KeyStroke>({ down(0xA2, HostCtrl), down(0xA4, HostCtrl | HostAlt),
                                           down(0x2E, HostCtrl | HostAlt | HostExtended),
                                           up(0x2E, HostCtrl | HostAlt | HostExtended),
                                           up(0xA4, HostCtrl), up(0xA2, 0) }),
                  QStringLiteral("Ctrl+Alt+Del strokes changed"), err);
    ok &=
        require(strokes(special("win")) == QVector<KeyStroke>({ down(0x5B, HostMeta | HostExtended),
                                                                up(0x5B, HostExtended) }),
                QStringLiteral("Win strokes changed"), err);
    ok &= require(strokes(special("win-ctrl-o")) ==
                      QVector<KeyStroke>(
                          { down(0xA2, HostCtrl), down(0x5B, HostCtrl | HostMeta | HostExtended),
                            down(0x4F, HostCtrl | HostMeta), up(0x4F, HostCtrl | HostMeta),
                            up(0x5B, HostCtrl | HostExtended), up(0xA2, 0) }),
                  QStringLiteral("Win+Ctrl+O strokes changed"), err);
    ok &= require(strokes(special("print-screen")) ==
                      QVector<KeyStroke>({ down(0x2C, HostExtended), up(0x2C, HostExtended) }),
                  QStringLiteral("Print Screen strokes changed"), err);
    ok &= require(strokes(ShortcutBinding(0, ShortcutKeys::Mute)).isEmpty(),
                  QStringLiteral("unsendable combo produced strokes"), err);

    // Every built-in sequence releases what it presses, in reverse order
    QSet<QString> ids;
    for (const SpecialKey& key : specialKeys()) {
        ids.insert(QLatin1String(key.id));
        const QVector<KeyStroke> sequence = strokes(key.binding);
        QVector<quint16> held;
        bool balanced = !sequence.isEmpty();
        for (const KeyStroke& stroke : sequence) {
            if (stroke.down) {
                held.append(stroke.keyCode);
            } else {
                balanced &= !held.isEmpty() && held.takeLast() == stroke.keyCode;
            }
        }
        balanced &= held.isEmpty() && (sequence.last().modifiers & ~HostExtended) == 0;
        ok &= require(balanced, QStringLiteral("%1 is not balanced").arg(key.id), err);
        ok &= require(bindingForPayload(specialKeyPayload(key)) == key.binding,
                      QStringLiteral("%1 payload did not round-trip").arg(key.id), err);
    }
    ok &= require(specialKeys().size() == 7 && ids.size() == 7,
                  QStringLiteral("special key list changed"), err);
    return ok;
}

bool testDisplayText(QTextStream& err)
{
    bool ok = true;
    ok &= require(HostKeys::displayText(special(HostKeys::CtrlAltDel)) ==
                      QLatin1String("Ctrl+Alt+Delete"),
                  QStringLiteral("Ctrl+Alt+Del text changed"), err);
    ok &= require(HostKeys::displayText(special("win")) == QLatin1String("Win"),
                  QStringLiteral("lone Win text changed"), err);
    ok &= require(HostKeys::displayText(special("win-ctrl-o")) == QLatin1String("Ctrl+Win+O"),
                  QStringLiteral("Gui is not shown as Win"), err);
    ok &= require(HostKeys::displayText(ShortcutBinding()).isEmpty(),
                  QStringLiteral("null combo has text"), err);
    return ok;
}

bool testMacroValidity(QTextStream& err)
{
    bool ok = true;
    using HostKeys::Macro;

    const ShortcutBinding f5(ShortcutBinding::ModNone, ShortcutKeys::F1 + 4);
    ok &= require(HostKeys::isValidMacro(Macro{ QStringLiteral("Refresh"), f5 }),
                  QStringLiteral("bare F5 macro rejected"), err);
    ok &= require(!HostKeys::isValidMacro(Macro{ QStringLiteral("  \n "), f5 }),
                  QStringLiteral("blank name accepted"), err);
    ok &= require(!HostKeys::isValidMacro(Macro{ QStringLiteral("Win"), special("win") }),
                  QStringLiteral("lone modifier macro accepted"), err);
    ok &= require(!HostKeys::isValidMacro(
                      Macro{ QStringLiteral("Mute"), ShortcutBinding(0, ShortcutKeys::Mute) }),
                  QStringLiteral("unsendable macro accepted"), err);
    ok &= require(HostKeys::normalizedName(QStringLiteral("  Save\n  all ")) ==
                      QLatin1String("Save all"),
                  QStringLiteral("name not normalized"), err);
    ok &= require(HostKeys::normalizedName(QString(100, QLatin1Char('x'))).size() ==
                      HostKeys::MaxNameLength,
                  QStringLiteral("long name not cut"), err);

    const Macro saveAll{ QStringLiteral("Save all"),
                         ShortcutBinding(ShortcutBinding::ModCtrl | ShortcutBinding::ModShift,
                                         ShortcutKeys::letter('S')) };
    ok &= require(HostKeys::bindingForPayload(HostKeys::macroPayload(saveAll)) == saveAll.binding,
                  QStringLiteral("macro payload did not round-trip"), err);
    ok &= require(HostKeys::bindingForPayload(QStringLiteral("special:nope")).isNull() &&
                      HostKeys::bindingForPayload(QStringLiteral("keys:ctrl+Bogus")).isNull() &&
                      HostKeys::bindingForPayload(QStringLiteral("ctrl+alt+Delete")).isNull(),
                  QStringLiteral("bad payload accepted"), err);
    return ok;
}

bool testMacroPersistence(QTextStream& err)
{
    bool ok = true;
    using HostKeys::Macro;

    QTemporaryDir dir;
    ok &= require(dir.isValid(), QStringLiteral("no temporary directory"), err);
    const QString path = dir.filePath(QStringLiteral("macros.ini"));

    const Macro first{ QStringLiteral("Close window"),
                       ShortcutBinding(ShortcutBinding::ModAlt, ShortcutKeys::F1 + 3) };
    const Macro second{ QStringLiteral("Lock"),
                        ShortcutBinding(ShortcutBinding::ModGui, ShortcutKeys::letter('L')) };
    const Macro invalid{ QString(), second.binding };
    {
        QSettings settings(path, QSettings::IniFormat);
        HostKeys::saveMacros(settings, { first, invalid, second });
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        ok &= require(HostKeys::loadMacros(settings) == QVector<Macro>({ first, second }),
                      QStringLiteral("macros did not round-trip"), err);
        ok &= require(settings.value(QStringLiteral("keyMacros/macros/1/keys")).toString() ==
                          QLatin1String("alt+F4"),
                      QStringLiteral("stored macro format changed"), err);
    }

    // At most MaxMacros are kept
    QVector<Macro> many;
    for (int i = 0; i < HostKeys::MaxMacros + 2; i++) {
        many.append(Macro{ QStringLiteral("Macro %1").arg(i),
                           ShortcutBinding(k_CtrlAlt, ShortcutKeys::letter('A') + i) });
    }
    {
        QSettings settings(path, QSettings::IniFormat);
        HostKeys::saveMacros(settings, many);
        ok &= require(HostKeys::loadMacros(settings) == many.mid(0, HostKeys::MaxMacros),
                      QStringLiteral("macro limit not applied on save"), err);

        // A shorter list replaces the longer one completely
        HostKeys::saveMacros(settings, { second });
        ok &= require(HostKeys::loadMacros(settings) == QVector<Macro>({ second }) &&
                          !settings.contains(QStringLiteral("keyMacros/macros/2/name")),
                      QStringLiteral("shorter list left old entries"), err);
    }

    // Hand-edited or foreign entries are skipped when loading
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.remove(QStringLiteral("keyMacros"));
        settings.beginGroup(QStringLiteral("keyMacros"));
        settings.beginWriteArray(QStringLiteral("macros"));
        const QStringList keys = { QStringLiteral("ctrl+Bogus"), QStringLiteral("gui+L"),
                                   QStringLiteral("Mute"), QStringLiteral("alt+F4") };
        for (int i = 0; i < keys.size(); i++) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("name"), i == 3 ? QString() : first.name);
            settings.setValue(QStringLiteral("keys"), keys[i]);
        }
        settings.endArray();
        settings.endGroup();
        ok &= require(HostKeys::loadMacros(settings) ==
                          QVector<Macro>({ Macro{ first.name, second.binding } }),
                      QStringLiteral("invalid stored entries were loaded"), err);
    }
    return ok;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testVirtualKeys(err);
    ok &= testStrokes(err);
    ok &= testDisplayText(err);
    ok &= testMacroValidity(err);
    ok &= testMacroPersistence(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
