QT += core gui qml svg testlib
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = menu_phase1
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp \
    ../../app/streaming/video/overlaymenugeometry.cpp \
    ../../app/streaming/input/shortcuts.cpp ../../app/streaming/streamquality.cpp
HEADERS += ../../app/streaming/video/overlayrasterwindow.h \
    ../../app/streaming/video/overlaymenupanel.h \
    ../../app/streaming/video/overlaymenugeometry.h ../../app/streaming/input/shortcuts.h
RESOURCES += ../overlay_menu_navigation/fonts.qrc icons.qrc
