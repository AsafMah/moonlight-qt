QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = present_throttle
INCLUDEPATH += ../../app
SOURCES += main.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/pacer/presentthrottledetector.h
