QT += core
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

TARGET = pyrowave_frame_feed
TEMPLATE = app

INCLUDEPATH += ../../app

SOURCES += \
    main.cpp

HEADERS += \
    ../../app/streaming/video/pyrowave/pyrowaveframefeed.h
