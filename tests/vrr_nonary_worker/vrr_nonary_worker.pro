# Ported from Nonary/moonlight-qt@c822745 tests/vrr/pacingworker.pro (GPLv3).
# MODIFIED 2026-10-30: own test directory, shared library block.
TEMPLATE = app
TARGET = vrr_nonary_worker
QT += core qml
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES += SDL_MAIN_HANDLED
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += tst_vrrpacingworker.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingtrace.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp \
    ../../app/streaming/vrrratepolicy.cpp
HEADERS += vrrtestfakes.h

win32 {
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/x64 \
                       $$PWD/../../libs/windows/include/x64/SDL2
        LIBS += -L$$PWD/../../libs/windows/lib/x64 -lavutil -lSDL2
        # Run without the libs directory on PATH (a missing DLL is a modal dialog)
        QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL*.dll) $$shell_path($$OUT_PWD/release/) $$escape_expand(\n\t)
        QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/avutil-*.dll) $$shell_path($$OUT_PWD/release/)
    }
    contains(QT_ARCH, arm64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/arm64 \
                       $$PWD/../../libs/windows/include/arm64/SDL2
        LIBS += -L$$PWD/../../libs/windows/lib/arm64 -lavutil -lSDL2
    }
}
unix:!macx {
    CONFIG += link_pkgconfig
    PKGCONFIG += libavutil sdl2
}
