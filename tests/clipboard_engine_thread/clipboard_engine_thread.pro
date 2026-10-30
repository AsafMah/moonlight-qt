QT += core gui network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = clipboard_engine_thread
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../clipboard-helper

SOURCES += \
    main.cpp \
    ../../app/streaming/clipboardsync.cpp

macx:SOURCES += ../../clipboard-helper/macos.mm

HEADERS += \
    ../../app/streaming/clipboardlogging.h \
    ../../app/streaming/clipboardsync.h \
    ../../clipboard-helper/clipboardhelperthreading.h
