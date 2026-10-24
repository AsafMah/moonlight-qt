QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = shortcuts
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/input/shortcuts.cpp

HEADERS += \
    ../../app/streaming/input/shortcuts.h
