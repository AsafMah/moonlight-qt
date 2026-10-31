QT += core gui svg testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
!system(python test_live_window_mode.py --generate): error(Failed to extract production window mode methods)
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp \
    ../../app/streaming/video/overlaymenugeometry.cpp \
    ../../app/streaming/input/shortcuts.cpp ../../app/streaming/streamquality.cpp
HEADERS += ../../app/streaming/video/overlayrasterwindow.h \
    ../../app/streaming/video/overlaymenupanel.h \
    ../../app/streaming/video/overlaymenugeometry.h \
    ../../app/streaming/input/shortcuts.h
RESOURCES += ../overlay_menu_navigation/fonts.qrc
