QT += core gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = stats_overlay_layout
TEMPLATE = app

INCLUDEPATH += $$PWD/../../app $$PWD/../../moonlight-common-c/moonlight-common-c/src
win32 {
    INCLUDEPATH += $$PWD/../../libs/windows/include/x64/SDL2
    LIBS += $$PWD/../../libs/windows/lib/x64/SDL2.lib
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL2.dll) $$shell_path($$OUT_PWD/release/SDL2.dll)
    QMAKE_POST_LINK += $$escape_expand(\\n\\t) $$QMAKE_COPY $$shell_path($$PWD/../../libs/windows/lib/x64/SDL3.dll) $$shell_path($$OUT_PWD/release/SDL3.dll)
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2 SDL2_ttf
}

# The real font test loads the fonts bundled with the app
DEFINES += SOURCE_DIR=\\\"$$PWD\\\"
DEFINES += TEST_TRANSLATIONS_DIR=\\\"$$OUT_PWD/release\\\"
TEST_TRANSLATIONS += $$PWD/../../app/languages/qml_zh_CN.ts $$PWD/../../app/languages/qml_zh_TW.ts
test_translations.input = TEST_TRANSLATIONS
test_translations.output = release/${QMAKE_FILE_BASE}.qm
test_translations.commands = $$shell_quote($$shell_path($$[QT_HOST_BINS]/lrelease)) ${QMAKE_FILE_IN} -qm ${QMAKE_FILE_OUT}
test_translations.CONFIG += no_link target_predeps
QMAKE_EXTRA_COMPILERS += test_translations

SOURCES += \
    main.cpp \
    panel_layout_tests.cpp \
    window_render_tests.cpp \
    controller_backend_fake.cpp \
    ../../app/streaming/stats/statsmath.cpp \
    ../../app/streaming/stats/streamstatsmodel.cpp \
    ../../app/streaming/video/overlaystatscontroller.cpp \
    ../../app/streaming/video/overlaystatstooltip.cpp \
    ../../app/settings/statsoverlaysettings.cpp \
    ../../app/streaming/video/overlaystatspreset.cpp \
    ../../app/streaming/video/overlaystatslayout.cpp \
    ../../app/streaming/video/overlaystatswindow.cpp
SOURCES += ../../app/streaming/video/overlaystatsstrip.cpp

HEADERS += \
    panel_layout_tests.h \
    ../../app/settings/statsoverlaysettings.h \
    ../../app/streaming/video/overlayrasterwindow.h \
    ../../app/streaming/video/overlaystatspreset.h \
    ../../app/streaming/video/overlaystatslayout.h \
    ../../app/streaming/video/overlaystatsfonts.h \
    ../../app/streaming/video/overlaystatswindow.h
HEADERS += ../../app/streaming/video/overlaystatstooltip.h
