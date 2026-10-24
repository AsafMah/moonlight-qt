QT += core gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = stats_overlay_layout
TEMPLATE = app

INCLUDEPATH +=     $$PWD/../../app

# The real font test loads the fonts bundled with the app
DEFINES += SOURCE_DIR=\\\"$$PWD\\\"

SOURCES +=     main.cpp     panel_layout_tests.cpp     ../../app/streaming/video/overlaystatspreset.cpp     ../../app/streaming/video/overlaystatslayout.cpp

HEADERS +=     panel_layout_tests.h     ../../app/streaming/video/overlaystatspreset.h     ../../app/streaming/video/overlaystatslayout.h     ../../app/streaming/video/overlaystatsfonts.h
