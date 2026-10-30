QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = dxgi_present_queue
INCLUDEPATH += ../../app
SOURCES += main.cpp
HEADERS += ../../app/streaming/video/ffmpeg-renderers/d3d11presentpolicy.h
