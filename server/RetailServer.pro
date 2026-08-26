# Server project configuration (hybrid architecture: Qt GUI + epoll I/O)
QT       += core gui widgets network sql

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = RetailServer
TEMPLATE = app

# Output directory (use $$PWD for an absolute path so Qt Creator finds it)
DESTDIR = $$PWD/../build/server
OBJECTS_DIR = $$PWD/../build/server/.obj
MOC_DIR = $$PWD/../build/server/.moc
UI_DIR = $$PWD/../build/server/.ui

# Header search paths
INCLUDEPATH += ../common
INCLUDEPATH += ../common/cjson
INCLUDEPATH += ./core
INCLUDEPATH += ./ui

# Sources
SOURCES += \
    main.cpp \
    ui/mainwindow.cpp \
    core/databasemanager.cpp \
    core/servermanager.cpp \
    core/businessmanager.cpp \
    core/cframeworkadapter.cpp \
    ui/memberwidget.cpp \
    ui/goodswidget.cpp \
    ui/orderwidget.cpp \
    ui/clientwidget.cpp \
    ui/otawidget.cpp \
    ui/stylehelper.cpp \
    ui/monitor_widget.cpp \
    ui/video_player_widget.cpp \
    core/stream_receiver.cpp \
    core/thread_pool.c \
    core/net_framework.c \
    core/nf_epoll.c \
    ../common/cjson/cJSON.c \
    ../common/crypto.c

# Headers
HEADERS += \
    ui/mainwindow.h \
    core/databasemanager.h \
    core/servermanager.h \
    core/businessmanager.h \
    core/cframeworkadapter.h \
    core/thread_pool.h \
    core/net_framework.h \
    core/nf_epoll.h \
    core/nf_iouring.h \
    core/stream_receiver.h \
    ui/memberwidget.h \
    ui/goodswidget.h \
    ui/orderwidget.h \
    ui/clientwidget.h \
    ui/otawidget.h \
    ui/stylehelper.h \
    ui/monitor_widget.h \
    ui/video_player_widget.h \
    ../common/cjson/cJSON.h \
    ../common/crypto.h \
    ../common/common.h

# UI files
FORMS += \
    ui/mainwindow.ui \
    ui/memberwidget.ui \
    ui/goodswidget.ui \
    ui/orderwidget.ui \
    ui/clientwidget.ui \
    ui/otawidget.ui

# Linked libraries
LIBS += -lpthread -lssl -lcrypto

# FFmpeg (video decoding / monitor display)
FFMPEG_EXISTS = $$system(pkg-config --exists libavcodec 2>/dev/null && echo yes || echo no)
equals(FFMPEG_EXISTS, "yes") {
    DEFINES += USE_FFMPEG
    LIBS += -lavcodec -lavformat -lavutil -lswscale
    message("FFmpeg: found, video decoding/monitor enabled")
} else {
    message("FFmpeg: not found, monitor disabled")
}

# Configuration
CONFIG += c++11
DEFINES += QT_DEPRECATED_WARNINGS

# io_uring backend (requires liburing; run ./scripts/setup_liburing.sh to install)
DEFINES += USE_IO_URING
SOURCES += core/nf_iouring.c

# Link liburing (prefer pkg-config, fall back to vendored copy under 3rdparty/)
LIBURING_DIR = $$PWD/../3rdparty/liburing/usr/local
LIBURING_PKG = $$system(pkg-config --exists liburing 2>/dev/null && echo yes || echo no)
equals(LIBURING_PKG, "yes") {
    QMAKE_CXXFLAGS += $$system(pkg-config --cflags liburing)
    QMAKE_CFLAGS   += $$system(pkg-config --cflags liburing)
    LIBS += $$system(pkg-config --libs liburing)
} else {
    INCLUDEPATH += $$LIBURING_DIR/include
    QMAKE_LIBDIR += $$LIBURING_DIR/lib
    LIBS += -luring
    QMAKE_RPATHDIR += $$LIBURING_DIR/lib
}
LIBS += -lcrypt
