QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = stats_model
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/bandwidth.cpp \
    ../../app/streaming/stats/statsmath.cpp \
    ../../app/streaming/stats/streamstatsmodel.cpp

HEADERS += \
    ../../app/streaming/stats/statsmath.h \
    ../../app/streaming/stats/statssnapshot.h \
    ../../app/streaming/stats/streamstatsmodel.h
HEADERS += \
    ../../app/streaming/bandwidth.h
