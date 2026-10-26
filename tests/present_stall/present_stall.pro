QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = present_stall
INCLUDEPATH += ../../app
SOURCES += main.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/pacer/presentstalldetector.h
