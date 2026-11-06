QT += core network
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = display_group
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += main.cpp \
    hub_tests.cpp \
    sink_tests.cpp \
    ../../app/streaming/displaygroup.cpp \
    ../../app/streaming/displaygroupipc.cpp \
    ../../app/streaming/input/keyboardsink.cpp
HEADERS += ../../app/streaming/displaygroup.h \
    ../../app/streaming/displaygroupipc.h \
    ../../app/streaming/input/keyboardsink.h
