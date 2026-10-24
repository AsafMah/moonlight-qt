QT += core
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

TARGET = pyrowave_pacing
TEMPLATE = app

INCLUDEPATH += ../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/video/pyrowave/pyrowavepacing.cpp

HEADERS += \
    ../../app/streaming/video/pyrowave/pyrowavepacing.h
