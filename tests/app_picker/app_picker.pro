QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = app_picker
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/gui/apppicker.cpp

HEADERS += \
    ../../app/gui/apppicker.h \
    ../../app/backend/nvapp.h
