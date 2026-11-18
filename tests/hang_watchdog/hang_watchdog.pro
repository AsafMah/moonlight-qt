QT += core widgets
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = hang_watchdog
INCLUDEPATH += ../../app ../../libs/windows/include/x64/SDL2
SOURCES += watchdog_test.cpp ../../app/hangwatchdog.cpp
LIBS += dbghelp.lib -L../../libs/windows/lib/x64 SDL2.lib
DEFINES += SDL_MAIN_HANDLED
DEFINES += MOONLIGHT_HANG_WATCHDOG_TESTING
contains(CONFIG, quick): DEFINES += MOONLIGHT_HANG_TEST_THRESHOLD_MS=400
contains(CONFIG, quick): DEFINES += MOONLIGHT_HANG_TEST_WRITER_TIMEOUT_MS=1200
