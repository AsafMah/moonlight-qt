QT += core gui svg testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/streaming/video/overlaymenupanel.cpp \
    ../../app/streaming/input/shortcuts.cpp \
    ../../app/streaming/streamquality.cpp
HEADERS += \
    ../../app/streaming/video/overlayrasterwindow.h \
    ../../app/streaming/video/overlaymenupanel.h \
    ../../app/streaming/input/shortcuts.h
RESOURCES += fonts.qrc
