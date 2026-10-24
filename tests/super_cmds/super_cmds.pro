QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = super_cmds
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/backend/nvapp.cpp \
    ../../app/backend/supercmds.cpp

HEADERS += \
    ../../app/backend/nvapp.h \
    ../../app/backend/supercmds.h
