QT += core
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

TARGET = pyrowave_color
TEMPLATE = app

INCLUDEPATH += \
    ../../app \
    ../../moonlight-common-c/moonlight-common-c/src

# The libplacebo mapping uses only libplacebo's header structs, so nothing is
# linked. Without the prebuilt headers the rest is still tested.
win32 {
    contains(QT_ARCH, arm64) {
        PLACEBO_INCLUDE = $$PWD/../../libs/windows/include/arm64
    } else {
        PLACEBO_INCLUDE = $$PWD/../../libs/windows/include/x64
    }
    exists($$PLACEBO_INCLUDE/libplacebo/colorspace.h) {
        INCLUDEPATH += $$PLACEBO_INCLUDE
        DEFINES += HAVE_LIBPLACEBO_VULKAN
    }
}
unix:packagesExist(libplacebo) {
    CONFIG += link_pkgconfig
    PKGCONFIG += libplacebo
    DEFINES += HAVE_LIBPLACEBO_VULKAN
}

SOURCES += \
    main.cpp \
    ../../app/streaming/video/pyrowave/pyrowavecolor.cpp

HEADERS += \
    ../../app/streaming/video/pyrowave/pyrowavecolor.h
