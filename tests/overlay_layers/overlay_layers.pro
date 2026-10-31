QT += core gui
CONFIG += console c++17
CONFIG -= app_bundle

TARGET = overlay_layers
TEMPLATE = app

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/video/overlaybitmaplayers.cpp

HEADERS += \
    ../../app/streaming/video/overlaybitmaplayers.h
