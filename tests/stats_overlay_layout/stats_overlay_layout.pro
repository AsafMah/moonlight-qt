QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = stats_overlay_layout
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/video/overlaystatspreset.cpp

HEADERS += \
    ../../app/streaming/video/overlaystatspreset.h
