QT += core gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = shortcuts
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/gui/shortcutmodel.cpp \
    ../../app/streaming/input/shortcuts.cpp

HEADERS += \
    ../../app/streaming/input/mousereleasegesture.h \
    ../../app/gui/shortcutmodel.h \
    ../../app/streaming/input/shortcuts.h
