QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = vrr_engagement
INCLUDEPATH += ../../app
SOURCES += main.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/pacer/vrrengagementdetector.h
