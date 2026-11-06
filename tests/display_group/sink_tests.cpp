// KeyboardSink: ordinary sessions call moonlight-common-c directly; a display
// group member's forwarder takes every key and text event instead. The
// common-c entry points are faked here so the default path is observable.

#include "streaming/input/keyboardsink.h"

#include <Limelight.h>

#include <QByteArray>
#include <QTextStream>
#include <QVector>

namespace {
struct LiCall
{
    short code;
    char action, modifiers, flags;
};
QVector<LiCall> s_LiKeys;
QVector<QByteArray> s_LiTexts;

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: sink: " << message << '\n';
    }
    return condition;
}
} // namespace

extern "C" int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags)
{
    s_LiKeys.append({ keyCode, keyAction, modifiers, flags });
    return 0;
}

extern "C" int LiSendUtf8TextEvent(const char* text, unsigned int length)
{
    s_LiTexts.append(QByteArray(text, int(length)));
    return 0;
}

int runSinkTests(QTextStream& err)
{
    bool ok = true;

    ok &= require(!KeyboardSink::isForwarding(), "no forwarder by default", err);
    KeyboardSink::sendKeyboardEvent2(short(0x8041), KEY_ACTION_DOWN, MODIFIER_SHIFT, 0x01);
    KeyboardSink::sendUtf8TextEvent("abc", 3);
    ok &= require(s_LiKeys.size() == 1 && s_LiKeys[0].code == short(0x8041) &&
                      s_LiKeys[0].action == KEY_ACTION_DOWN &&
                      s_LiKeys[0].modifiers == MODIFIER_SHIFT && s_LiKeys[0].flags == 0x01,
                  "an ordinary session sends keys to common-c unchanged", err);
    ok &= require(s_LiTexts.size() == 1 && s_LiTexts[0] == "abc",
                  "an ordinary session sends text to common-c unchanged", err);

    QVector<LiCall> forwarded;
    QVector<QByteArray> forwardedText;
    KeyboardSink::setForwarder(
        [&](short code, char action, char modifiers, char flags) {
            forwarded.append({ code, action, modifiers, flags });
            return 0;
        },
        [&](const char* text, unsigned int length) {
            forwardedText.append(QByteArray(text, int(length)));
            return 0;
        });
    ok &= require(KeyboardSink::isForwarding(), "forwarder installed", err);
    KeyboardSink::sendKeyboardEvent2(short(0x8042), KEY_ACTION_UP, 0, 0);
    KeyboardSink::sendUtf8TextEvent("xy", 2);
    ok &= require(forwarded.size() == 1 && forwarded[0].code == short(0x8042) &&
                      forwarded[0].action == KEY_ACTION_UP,
                  "a member forwards keys", err);
    ok &= require(forwardedText.size() == 1 && forwardedText[0] == "xy", "a member forwards text",
                  err);
    ok &= require(s_LiKeys.size() == 1 && s_LiTexts.size() == 1,
                  "a member sends nothing on its own session", err);

    KeyboardSink::clearForwarder();
    KeyboardSink::sendKeyboardEvent2(short(0x8043), KEY_ACTION_DOWN, 0, 0);
    ok &= require(s_LiKeys.size() == 2 && forwarded.size() == 1,
                  "clearing restores the direct path", err);
    return ok ? 0 : 1;
}
