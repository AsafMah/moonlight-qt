QT += core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = session_summary
TEMPLATE = app

INCLUDEPATH += \
    $$PWD/../../app

SOURCES += \
    main.cpp \
    ../../app/streaming/stats/sessionstatsrecorder.cpp \
    ../../app/streaming/stats/sessionsummary.cpp \
    ../../app/streaming/stats/statsmath.cpp \
    ../../app/streaming/stats/streamstatsmodel.cpp

HEADERS += \
    ../../app/streaming/stats/sessionstatsrecorder.h \
    ../../app/streaming/stats/sessionsummary.h \
    ../../app/streaming/stats/statsmath.h \
    ../../app/streaming/stats/statssnapshot.h \
    ../../app/streaming/stats/streamstatsmodel.h
msvc: QMAKE_CXXFLAGS += /utf-8
