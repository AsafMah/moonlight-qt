#include "streaming/input/keyboardsink.h"

#include <Limelight.h>

#include <mutex>

namespace KeyboardSink {

namespace {
std::mutex s_Lock;
KeyForwarder s_Key;
TextForwarder s_Text;
} // namespace

void setForwarder(KeyForwarder key, TextForwarder text)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Key = std::move(key);
    s_Text = std::move(text);
}

void clearForwarder()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Key = nullptr;
    s_Text = nullptr;
}

bool isForwarding()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    return static_cast<bool>(s_Key);
}

int sendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags)
{
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        if (s_Key) {
            return s_Key(keyCode, keyAction, modifiers, flags);
        }
    }
    return LiSendKeyboardEvent2(keyCode, keyAction, modifiers, flags);
}

int sendUtf8TextEvent(const char* utf8Text, unsigned int length)
{
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        if (s_Text) {
            return s_Text(utf8Text, length);
        }
    }
    return LiSendUtf8TextEvent(utf8Text, length);
}

} // namespace KeyboardSink
