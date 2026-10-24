QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = stream_quality
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/streamquality.cpp

HEADERS += \
    ../../app/streaming/streamquality.h
