#pragma once

#include <functional>

// Single exit point for keyboard input sent to the host. Ordinary sessions go
// straight to moonlight-common-c. A display group member installs a forwarder
// so its keys are sent on the leader's session (the host has one keyboard).
namespace KeyboardSink {

using KeyForwarder = std::function<int(short keyCode, char keyAction, char modifiers, char flags)>;
using TextForwarder = std::function<int(const char* utf8Text, unsigned int length)>;

// Install before the stream starts and clear after it ends.
void setForwarder(KeyForwarder key, TextForwarder text);
void clearForwarder();
bool isForwarding();

int sendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags);
int sendUtf8TextEvent(const char* utf8Text, unsigned int length);

} // namespace KeyboardSink
