#include "streaming/input/shortcuts.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

const quint8 k_CtrlAltShift =
    ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt | ShortcutBinding::ModShift;
const quint8 k_CtrlAlt = ShortcutBinding::ModCtrl | ShortcutBinding::ModAlt;

ShortcutBinding key(quint8 modifiers, char letter)
{
    return ShortcutBinding(modifiers, ShortcutKeys::letter(letter));
}

bool testTokens(QTextStream& err)
{
    bool ok = true;

    for (int scancode = 1; scancode < ShortcutKeys::MaxScancode; scancode++) {
        const QString token = ShortcutKeys::toToken(scancode);
        ok &= require(
            !token.isEmpty() && !token.contains(QLatin1Char('+')) &&
                !token.contains(QLatin1Char(',')) && !token.contains(QLatin1Char(' ')),
            QStringLiteral("token for scancode %1 is not storable: '%2'").arg(scancode).arg(token),
            err);
        ok &= require(
            ShortcutKeys::fromToken(token) == scancode,
            QStringLiteral("token '%1' did not round-trip to scancode %2").arg(token).arg(scancode),
            err);
    }

    ok &= require(ShortcutKeys::toToken(ShortcutKeys::letter('S')) == QLatin1String("S"),
                  QStringLiteral("letter token changed"), err);
    ok &= require(ShortcutKeys::toToken(ShortcutKeys::F13) == QLatin1String("F13"),
                  QStringLiteral("F13 token changed"), err);
    ok &= require(ShortcutKeys::toToken(ShortcutKeys::Num0) == QLatin1String("0"),
                  QStringLiteral("digit token changed"), err);
    ok &= require(ShortcutKeys::toToken(200) == QLatin1String("sc200"),
                  QStringLiteral("unnamed scancode token changed"), err);
    ok &= require(ShortcutKeys::fromToken(QStringLiteral("pageup")) == 75,
                  QStringLiteral("token lookup is not case-insensitive"), err);
    ok &= require(ShortcutKeys::fromToken(QStringLiteral("sc0")) == ShortcutKeys::Unknown &&
                      ShortcutKeys::fromToken(QStringLiteral("sc9999")) == ShortcutKeys::Unknown &&
                      ShortcutKeys::fromToken(QStringLiteral("Bogus")) == ShortcutKeys::Unknown,
                  QStringLiteral("bad tokens were accepted"), err);

    return ok;
}

bool testParseAndFormat(QTextStream& err)
{
    bool ok = true;

    const ShortcutBinding stats = key(k_CtrlAltShift, 'S');
    ok &= require(stats.toString() == QLatin1String("ctrl+alt+shift+S"),
                  QStringLiteral("binding format changed: %1").arg(stats.toString()), err);
    ok &= require(ShortcutBinding::fromString(QStringLiteral("ctrl+alt+shift+S")) == stats,
                  QStringLiteral("canonical binding did not parse"), err);
    ok &= require(ShortcutBinding::fromString(QStringLiteral(" Shift+CTRL+alt+s ")) == stats,
                  QStringLiteral("modifier order or case changed the parse result"), err);
    ok &= require(ShortcutBinding::fromString(QStringLiteral("meta+F1")) ==
                      ShortcutBinding(ShortcutBinding::ModGui, ShortcutKeys::F1),
                  QStringLiteral("meta alias did not parse as gui"), err);
    ok &= require(ShortcutBinding::fromString(QStringLiteral("F13")) ==
                      ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F13),
                  QStringLiteral("bare key did not parse"), err);

    const char* const malformed[] = { "",        "ctrl+",          "ctrl+Bogus", "hyper+S",
                                      "ctrl++S", "ctrl+alt+sc224", "+S" };
    for (const char* string : malformed) {
        ok &= require(ShortcutBinding::fromString(QLatin1String(string)).isNull(),
                      QStringLiteral("malformed binding '%1' was accepted").arg(string), err);
    }

    ok &= require(ShortcutBinding().toString().isEmpty(),
                  QStringLiteral("null binding has a string form"), err);
    ok &= require(stats.toDisplayString() == QLatin1String("Ctrl+Alt+Shift+S"),
                  QStringLiteral("display string changed: %1").arg(stats.toDisplayString()), err);
    ok &= require(ShortcutBinding(k_CtrlAlt, 88).toDisplayString() ==
                      QLatin1String("Ctrl+Alt+Num Enter"),
                  QStringLiteral("keypad display string changed"), err);
    ok &=
        require(ShortcutBinding(k_CtrlAlt, 89).toDisplayString() == QLatin1String("Ctrl+Alt+Num 1"),
                QStringLiteral("generated keypad display string changed"), err);

    return ok;
}

bool testValidity(QTextStream& err)
{
    bool ok = true;

    ok &= require(key(ShortcutBinding::ModCtrl, 'S').isValid(),
                  QStringLiteral("Ctrl+S was rejected"), err);
    ok &= require(key(ShortcutBinding::ModGui, 'S').isValid(), QStringLiteral("Gui+S was rejected"),
                  err);
    ok &= require(!key(ShortcutBinding::ModShift, 'S').isValid(),
                  QStringLiteral("Shift+S was accepted"), err);
    ok &= require(!key(ShortcutBinding::ModNone, 'S').isValid(),
                  QStringLiteral("bare S was accepted"), err);
    ok &= require(!ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F1).isValid(),
                  QStringLiteral("bare F1 was accepted"), err);
    ok &= require(ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F13).isValid() &&
                      ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F24).isValid(),
                  QStringLiteral("bare F13/F24 were rejected"), err);
    ok &= require(ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::AudioPlay).isValid() &&
                      ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::VolumeUp).isValid(),
                  QStringLiteral("bare media keys were rejected"), err);
    ok &= require(!ShortcutBinding(k_CtrlAltShift, ShortcutKeys::LeftCtrl).isValid(),
                  QStringLiteral("a modifier key was accepted as the key"), err);
    ok &= require(!ShortcutBinding().isValid(), QStringLiteral("null binding was valid"), err);

    return ok;
}

bool testQtConversion(QTextStream& err)
{
    bool ok = true;

    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_S, 0) == ShortcutKeys::letter('S'),
                  QStringLiteral("Qt letter key fallback failed"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_F13, 0) == ShortcutKeys::F13,
                  QStringLiteral("Qt F13 fallback failed"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Exclam, 0) == ShortcutKeys::Num1,
                  QStringLiteral("shifted Qt key fallback failed"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_MediaTogglePlayPause, 0) ==
                      ShortcutKeys::AudioPlay,
                  QStringLiteral("Qt media key fallback failed"), err);

    // Latin letters follow the character, as the matcher does: the AZERTY key
    // labeled Q (physically QWERTY A) captures as Q. Other keys, including
    // non-latin letters (here Cyrillic ef, 0x444), use the native position.
    const int cyrillicEf = 0x0444;
#if defined(Q_OS_WIN32)
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Q, 0x1E) == ShortcutKeys::letter('Q'),
                  QStringLiteral("latin character was not preferred"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(cyrillicEf, 0x1E) == ShortcutKeys::letter('A'),
                  QStringLiteral("Windows scan code was not used"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Up, 0x148) == 82,
                  QStringLiteral("Windows extended scan code failed"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_F13, 0x64) == ShortcutKeys::F13,
                  QStringLiteral("Windows F13 scan code failed"), err);
#elif defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Q, 30 + 8) == ShortcutKeys::letter('Q'),
                  QStringLiteral("latin character was not preferred"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(cyrillicEf, 30 + 8) == ShortcutKeys::letter('A'),
                  QStringLiteral("evdev key code was not used"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Up, 103 + 8) == 82,
                  QStringLiteral("evdev arrow key failed"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_F13, 183 + 8) == ShortcutKeys::F13,
                  QStringLiteral("evdev F13 failed"), err);
#elif defined(Q_OS_DARWIN)
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_Q, 0x00) == ShortcutKeys::letter('Q'),
                  QStringLiteral("missing macOS key code did not fall back"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(Qt::Key_A, 0x0C) == ShortcutKeys::letter('A'),
                  QStringLiteral("latin character was not preferred"), err);
    ok &= require(ShortcutKeys::fromQtKeyEvent(cyrillicEf, 0x0C) == ShortcutKeys::letter('Q'),
                  QStringLiteral("macOS key code was not used"), err);
#else
    Q_UNUSED(cyrillicEf);
#endif

#ifndef Q_OS_DARWIN
    ok &= require(ShortcutBinding::fromQtModifiers(Qt::ControlModifier | Qt::ShiftModifier |
                                                   Qt::KeypadModifier) ==
                      (ShortcutBinding::ModCtrl | ShortcutBinding::ModShift),
                  QStringLiteral("Qt modifier conversion failed"), err);
    ok &= require(ShortcutBinding::fromQtModifiers(Qt::MetaModifier | Qt::AltModifier) ==
                      (ShortcutBinding::ModGui | ShortcutBinding::ModAlt),
                  QStringLiteral("Qt meta modifier conversion failed"), err);
#endif

    return ok;
}

bool testProfile(QTextStream& err)
{
    bool ok = true;
    ShortcutProfile profile;

    ok &= require(profile.bindings(QLatin1String(ShortcutActions::Quit)) ==
                      QVector<ShortcutBinding>({ key(k_CtrlAltShift, 'Q'), ShortcutBinding() }),
                  QStringLiteral("quit default changed"), err);
    ok &= require(profile.conflicts().isEmpty(), QStringLiteral("defaults conflict"), err);
    for (const ShortcutAction& action : profile.registry().actions()) {
        for (const ShortcutBinding& binding : profile.bindings(action.id)) {
            ok &= require(binding.isNull() || binding.isValid(),
                          QStringLiteral("default for %1 is invalid").arg(action.id), err);
        }
    }
    ok &= require(profile.bindings(QStringLiteral("no-such-action")) ==
                      QVector<ShortcutBinding>(ShortcutRegistry::MaxBindings),
                  QStringLiteral("unknown action has bindings"), err);
    // Host actions exist but stay off until bound
    for (const char* id : { ShortcutActions::SendCtrlAltDel, ShortcutActions::OpenHostCommands }) {
        ok &= require(profile.registry().find(QLatin1String(id)) != nullptr &&
                          profile.bindings(QLatin1String(id)) ==
                              QVector<ShortcutBinding>(ShortcutRegistry::MaxBindings),
                      QStringLiteral("%1 is missing or has a default").arg(QLatin1String(id)), err);
    }

    const QString mouse = QLatin1String(ShortcutActions::ToggleMouseMode);
    const QString stats = QLatin1String(ShortcutActions::ToggleStatsOverlay);
    const QString quit = QLatin1String(ShortcutActions::Quit);

    // Customize one action, then switch the prefix: only defaults follow it
    ok &= require(profile.setBinding(mouse, 0, ShortcutBinding(k_CtrlAlt, ShortcutKeys::F1)),
                  QStringLiteral("valid binding was rejected"), err);
    ok &= require(profile.isCustomized(mouse), QStringLiteral("customized flag missing"), err);
    profile.setDefaultPrefix(ShortcutProfile::PrefixCtrlAlt);
    ok &= require(profile.bindings(quit)[0] == key(k_CtrlAlt, 'Q'),
                  QStringLiteral("default did not follow the prefix"), err);
    ok &= require(profile.bindings(mouse)[0] == ShortcutBinding(k_CtrlAlt, ShortcutKeys::F1),
                  QStringLiteral("custom binding followed the prefix"), err);

    // Setting a binding back to the default drops the override
    ok &= require(profile.setBinding(mouse, 0, key(k_CtrlAlt, 'M')) && !profile.isCustomized(mouse),
                  QStringLiteral("default-equal binding stayed customized"), err);

    ok &= require(!profile.setBinding(mouse, 0, key(ShortcutBinding::ModShift, 'M')),
                  QStringLiteral("invalid binding was stored"), err);
    ok &= require(!profile.setBinding(mouse, ShortcutRegistry::MaxBindings, key(k_CtrlAlt, 'M')),
                  QStringLiteral("out of range slot was accepted"), err);
    ok &= require(!profile.setBinding(QStringLiteral("no-such-action"), 0, key(k_CtrlAlt, 'M')),
                  QStringLiteral("unknown action was accepted"), err);

    // Same binding twice in one action keeps only the new slot
    ok &= require(profile.setBinding(mouse, 1, key(k_CtrlAlt, 'M')),
                  QStringLiteral("duplicate slot binding was rejected"), err);
    ok &= require(profile.bindings(mouse) ==
                      QVector<ShortcutBinding>({ ShortcutBinding(), key(k_CtrlAlt, 'M') }),
                  QStringLiteral("duplicate slot was not cleared"), err);

    // Clearing every slot disables the action
    profile.setBinding(mouse, 1, ShortcutBinding());
    ok &= require(profile.isCustomized(mouse) &&
                      profile.bindings(mouse) ==
                          QVector<ShortcutBinding>(ShortcutRegistry::MaxBindings),
                  QStringLiteral("cleared action is not disabled"), err);
    profile.resetAction(mouse);
    ok &= require(!profile.isCustomized(mouse) && profile.bindings(mouse)[0] == key(k_CtrlAlt, 'M'),
                  QStringLiteral("reset did not restore the default"), err);

    // Conflicts are reported for both sides
    profile.setBinding(mouse, 1, key(k_CtrlAlt, 'S'));
    const QVector<ShortcutProfile::Conflict> conflicts = profile.conflicts();
    ok &= require(conflicts.size() == 1, QStringLiteral("expected one conflict"), err);
    if (conflicts.size() == 1) {
        const ShortcutProfile::Conflict& conflict = conflicts[0];
        ok &= require(conflict.binding == key(k_CtrlAlt, 'S') &&
                          ((conflict.firstAction == stats && conflict.firstSlot == 0 &&
                            conflict.secondAction == mouse && conflict.secondSlot == 1) ||
                           (conflict.firstAction == mouse && conflict.firstSlot == 1 &&
                            conflict.secondAction == stats && conflict.secondSlot == 0)),
                      QStringLiteral("conflict has the wrong slots"), err);
    }
    ok &= require(profile.conflictingActions(mouse, 1) == QStringList({ stats }) &&
                      profile.conflictingActions(stats, 0) == QStringList({ mouse }) &&
                      profile.conflictingActions(mouse, 0).isEmpty(),
                  QStringLiteral("conflicting actions are wrong"), err);

    profile.resetAll();
    ok &= require(profile.defaultPrefix() == ShortcutProfile::PrefixCtrlAltShift &&
                      !profile.isCustomized(mouse) && profile.conflicts().isEmpty(),
                  QStringLiteral("reset all left changes behind"), err);

    return ok;
}

bool testPersistence(QTextStream& err)
{
    bool ok = true;

    QTemporaryDir dir;
    if (!require(dir.isValid(), QStringLiteral("no temporary directory"), err)) {
        return false;
    }
    const QString path = dir.filePath(QStringLiteral("shortcuts.ini"));

    const QString mouse = QLatin1String(ShortcutActions::ToggleMouseMode);
    const QString stats = QLatin1String(ShortcutActions::ToggleStatsOverlay);
    const QString paste = QLatin1String(ShortcutActions::PasteText);
    const QString quit = QLatin1String(ShortcutActions::Quit);

    {
        QSettings settings(path, QSettings::IniFormat);
        // A key from another build must survive our save
        settings.setValue(QStringLiteral("shortcuts/bindings/other-build-action"),
                          QStringLiteral("ctrl+alt+O"));

        ShortcutProfile profile;
        profile.setDefaultPrefix(ShortcutProfile::PrefixCtrlAlt);
        profile.setBinding(mouse, 0, ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F13));
        profile.setBinding(mouse, 1, key(ShortcutBinding::ModGui, 'M'));
        profile.setBinding(stats, 0, ShortcutBinding());
        profile.setBinding(paste, 1, ShortcutBinding(k_CtrlAltShift, 52));
        profile.save(settings);
    }

    {
        QSettings settings(path, QSettings::IniFormat);
        ok &= require(settings.value(QStringLiteral("shortcuts/defaultPrefix")).toString() ==
                          QLatin1String("ctrl+alt"),
                      QStringLiteral("prefix was not stored as a string"), err);
        ok &= require(
            settings.value(QStringLiteral("shortcuts/bindings/other-build-action")).toString() ==
                QLatin1String("ctrl+alt+O"),
            QStringLiteral("foreign shortcut key was removed"), err);
        ok &= require(!settings.contains(QStringLiteral("shortcuts/bindings/") + quit),
                      QStringLiteral("default binding was written out"), err);

        ShortcutProfile loaded;
        loaded.load(settings);
        ok &= require(loaded.defaultPrefix() == ShortcutProfile::PrefixCtrlAlt,
                      QStringLiteral("prefix did not load"), err);
        ok &= require(loaded.bindings(mouse) ==
                          QVector<ShortcutBinding>(
                              { ShortcutBinding(ShortcutBinding::ModNone, ShortcutKeys::F13),
                                key(ShortcutBinding::ModGui, 'M') }),
                      QStringLiteral("two bindings did not load"), err);
        ok &= require(loaded.isCustomized(stats) &&
                          loaded.bindings(stats) ==
                              QVector<ShortcutBinding>(ShortcutRegistry::MaxBindings),
                      QStringLiteral("disabled action did not load"), err);
        ok &= require(loaded.bindings(paste) ==
                          QVector<ShortcutBinding>(
                              { key(k_CtrlAlt, 'V'), ShortcutBinding(k_CtrlAltShift, 52) }),
                      QStringLiteral("empty-free second slot did not load"), err);
        ok &= require(loaded.bindings(quit)[0] == key(k_CtrlAlt, 'Q'),
                      QStringLiteral("default did not use the loaded prefix"), err);

        // Hand-edited garbage only loses the bad slot
        settings.setValue(QStringLiteral("shortcuts/bindings/") + quit,
                          QStringLiteral("ctrl+alt+Bogus,ctrl+alt+F2"));
        loaded.load(settings);
        ok &= require(
            loaded.bindings(quit) ==
                QVector<ShortcutBinding>(
                    { ShortcutBinding(), ShortcutBinding(k_CtrlAlt, ShortcutKeys::F1 + 1) }),
            QStringLiteral("malformed slot handling failed"), err);
    }

    return ok;
}

bool testMatcher(QTextStream& err)
{
    bool ok = true;

    const QString stats = QLatin1String(ShortcutActions::ToggleStatsOverlay);
    const QString mouse = QLatin1String(ShortcutActions::ToggleMouseMode);
    const QString quit = QLatin1String(ShortcutActions::Quit);
    const int s = ShortcutKeys::letter('S');
    const int q = ShortcutKeys::letter('Q');
    const int a = ShortcutKeys::letter('A');
    const int none = ShortcutKeys::Unknown;
    const quint8 noMods = ShortcutBinding::ModNone;

    int statsCount = 0;
    int mouseCount = 0;
    int quitCount = 0;
    ShortcutDispatcher dispatcher;
    dispatcher.setHandler(stats, [&] { statsCount++; });
    dispatcher.setHandler(mouse, [&] { mouseCount++; });

    ShortcutProfile profile;
    ShortcutMatcher matcher;
    matcher.setProfile(profile);

    // On QWERTY the character and the position agree
    ok &= require(matcher.processKeyEvent(s, s, k_CtrlAltShift, true, false, dispatcher) &&
                      statsCount == 1,
                  QStringLiteral("shortcut did not fire"), err);
    ok &= require(matcher.processKeyEvent(s, s, k_CtrlAltShift, true, true, dispatcher) &&
                      statsCount == 1,
                  QStringLiteral("repeat was not swallowed or fired again"), err);
    // Modifiers released first: the key-up must still be swallowed
    ok &= require(matcher.processKeyEvent(s, s, noMods, false, false, dispatcher),
                  QStringLiteral("key-up was not swallowed"), err);
    ok &= require(!matcher.processKeyEvent(s, s, noMods, false, false, dispatcher),
                  QStringLiteral("second key-up was swallowed"), err);

    ok &=
        require(!matcher.processKeyEvent(s, s, ShortcutBinding::ModCtrl | ShortcutBinding::ModShift,
                                         true, false, dispatcher) &&
                    !matcher.processKeyEvent(s, s, k_CtrlAltShift | ShortcutBinding::ModGui, true,
                                             false, dispatcher) &&
                    statsCount == 1,
                QStringLiteral("modifier subset or superset matched"), err);
    ok &= require(!matcher.processKeyEvent(s, s, noMods, false, false, dispatcher),
                  QStringLiteral("unmatched key-up was swallowed"), err);

    ok &= require(!matcher.processKeyEvent(q, q, k_CtrlAltShift, true, false, dispatcher),
                  QStringLiteral("action without a handler was swallowed"), err);
    ok &= require(matcher.actionFor(key(k_CtrlAltShift, 'Q')) == quit &&
                      matcher.actionFor(key(k_CtrlAltShift, 'Q'), &dispatcher).isEmpty(),
                  QStringLiteral("actionFor handler filtering failed"), err);

    // A lost key-up (focus change) must not swallow the next plain press
    matcher.processKeyEvent(s, s, k_CtrlAltShift, true, false, dispatcher);
    ok &= require(!matcher.processKeyEvent(s, s, noMods, true, false, dispatcher) &&
                      !matcher.processKeyEvent(s, s, noMods, false, false, dispatcher),
                  QStringLiteral("stale swallowed key leaked into normal typing"), err);
    statsCount = 0;

    // AZERTY: the physical A key types "q" and must trigger Quit; its key-up
    // (same scancode) must be swallowed.
    dispatcher.setHandler(quit, [&] { quitCount++; });
    ok &= require(matcher.processKeyEvent(a, q, k_CtrlAltShift, true, false, dispatcher) &&
                      quitCount == 1,
                  QStringLiteral("AZERTY q did not trigger quit"), err);
    ok &= require(matcher.processKeyEvent(a, q, noMods, false, false, dispatcher),
                  QStringLiteral("AZERTY q key-up was not swallowed"), err);
    // The physical Q key types "a", which has no binding, so like the original
    // combo code it falls back to the Q position
    ok &= require(matcher.processKeyEvent(q, a, k_CtrlAltShift, true, false, dispatcher) &&
                      quitCount == 2,
                  QStringLiteral("unmatched character did not fall back to the scancode"), err);
    ok &= require(matcher.processKeyEvent(q, a, noMods, false, false, dispatcher),
                  QStringLiteral("scancode fallback key-up was not swallowed"), err);

    // Non-latin layout: no latin character, so the physical position matches
    ok &= require(matcher.processKeyEvent(q, none, k_CtrlAltShift, true, false, dispatcher) &&
                      quitCount == 3,
                  QStringLiteral("non-latin layout did not fall back to the scancode"), err);
    matcher.reset();

    // Collision: Mouse mode is bound to the A position as well. On AZERTY the
    // physical A key types "q", and the character pass must win, so it is
    // Quit (not Mouse mode via the scancode). The physical Q key types "a",
    // which is Mouse mode by character, even though Q is Quit's position.
    profile.setBinding(mouse, 1, key(k_CtrlAltShift, 'A'));
    matcher.setProfile(profile);
    ok &= require(matcher.processKeyEvent(a, q, k_CtrlAltShift, true, false, dispatcher) &&
                      quitCount == 4 && mouseCount == 0,
                  QStringLiteral("scancode binding shadowed a character binding"), err);
    ok &= require(matcher.processKeyEvent(q, a, k_CtrlAltShift, true, false, dispatcher) &&
                      quitCount == 4 && mouseCount == 1,
                  QStringLiteral("character binding lost to a scancode binding"), err);
    matcher.reset();
    profile.resetAll();
    dispatcher.removeHandler(quit);
    mouseCount = 0;

    // Conflicts: the first action that has a handler wins
    profile.setBinding(mouse, 1, key(k_CtrlAltShift, 'S'));
    dispatcher.removeHandler(stats);
    matcher.setProfile(profile);
    ok &= require(matcher.processKeyEvent(s, s, k_CtrlAltShift, true, false, dispatcher) &&
                      mouseCount == 1 && statsCount == 0,
                  QStringLiteral("conflict did not fall through to the handled action"), err);
    matcher.reset();

    // Disabled and bare bindings
    profile.resetAll();
    profile.setBinding(mouse, 0, ShortcutBinding());
    profile.setBinding(mouse, 1, ShortcutBinding(noMods, ShortcutKeys::F13));
    matcher.setProfile(profile);
    const int m = ShortcutKeys::letter('M');
    ok &= require(!matcher.processKeyEvent(m, m, k_CtrlAltShift, true, false, dispatcher),
                  QStringLiteral("disabled binding still fired"), err);
    ok &=
        require(matcher.processKeyEvent(ShortcutKeys::F13, none, noMods, true, false, dispatcher) &&
                    mouseCount == 2,
                QStringLiteral("bare F13 did not fire"), err);

    // The handler may reset or reconfigure the matcher while it runs
    dispatcher.setHandler(mouse, [&] { matcher.reset(); });
    ok &= require(matcher.processKeyEvent(ShortcutKeys::F13, none, noMods, true, false, dispatcher),
                  QStringLiteral("reentrant reset broke matching"), err);

    ok &=
        require(!dispatcher.trigger(QStringLiteral("no-such-action")) && dispatcher.trigger(mouse),
                QStringLiteral("dispatcher trigger result is wrong"), err);

    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testTokens(err);
    ok &= testParseAndFormat(err);
    ok &= testValidity(err);
    ok &= testQtConversion(err);
    ok &= testProfile(err);
    ok &= testPersistence(err);
    ok &= testMatcher(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
