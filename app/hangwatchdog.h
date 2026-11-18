#pragma once

// The helper entry point must run before Qt, SDL, or profile initialization.
namespace HangWatchdog {
int childMain(int argc, char** argv);
bool start(const wchar_t* dumpDirectory, const wchar_t* logPath);
void stop();
void qtHeartbeat();
void sdlHeartbeat();

class SdlScope {
public:
    SdlScope();
    ~SdlScope();
    SdlScope(const SdlScope&) = delete;
    SdlScope& operator=(const SdlScope&) = delete;
};

class Stage {
public:
    explicit Stage(const char* name);
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
private:
    int m_Slot = -1;
    char m_Previous[96] = {};
};
#ifdef MOONLIGHT_HANG_WATCHDOG_TESTING
unsigned long helperProcessId();
#endif
}
