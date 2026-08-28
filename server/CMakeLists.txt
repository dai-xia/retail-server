cmake_minimum_required(VERSION 3.14)
project(RetailServer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========== Qt ==========
find_package(Qt5 COMPONENTS Core Gui Widgets Network Sql REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# ========== Third-party dependencies ==========
find_package(OpenSSL REQUIRED)

# ========== FFmpeg (optional, video decoding / monitor display) ==========
# Always use find_library to get the absolute path and avoid -l link-order issues
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    HINTS /usr/include/x86_64-linux-gnu /usr/local/include /usr/include)
find_library(FFMPEG_AVCODEC avcodec)
find_library(FFMPEG_AVFORMAT avformat)
find_library(FFMPEG_AVUTIL avutil)
find_library(FFMPEG_SWSCALE swscale)
find_library(FFMPEG_SWRESAMPLE swresample)
if(FFMPEG_AVCODEC AND FFMPEG_AVFORMAT AND FFMPEG_AVUTIL AND FFMPEG_SWSCALE)
    set(FFMPEG_FOUND TRUE)
    set(FFMPEG_LIBRARIES ${FFMPEG_AVCODEC} ${FFMPEG_AVFORMAT} ${FFMPEG_AVUTIL} ${FFMPEG_SWSCALE} ${FFMPEG_SWRESAMPLE})
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})
    message(STATUS "Found FFmpeg: ${FFMPEG_LIBRARIES}")
    add_definitions(-DUSE_FFMPEG)
else()
    message(STATUS "FFmpeg not found, building without video decoding (monitor disabled)")
    set(FFMPEG_LIBRARIES "")
    set(FFMPEG_INCLUDE_DIRS "")
endif()

# ========== ALSA (audio playback on the monitor side) ==========
find_library(ALSA_LIBRARY asound)
if(ALSA_LIBRARY)
    message(STATUS "Found ALSA: ${ALSA_LIBRARY}")
else()
    message(STATUS "ALSA not found, monitor audio playback disabled")
endif()

# ========== liburing (prefer pkg-config, fall back to vendored 3rdparty) ==========
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBURING liburing)
endif()

if(NOT LIBURING_FOUND)
    set(LIBURING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/liburing/usr/local")
    if(EXISTS "${LIBURING_DIR}/include/liburing.h")
        set(LIBURING_INCLUDE_DIRS "${LIBURING_DIR}/include")
        set(LIBURING_LIBRARY_DIRS "${LIBURING_DIR}/lib")
        set(LIBURING_LIBRARIES "uring")
        set(LIBURING_RPATH "${LIBURING_DIR}/lib")
        message(STATUS "Using local liburing from 3rdparty")
    else()
        message(FATAL_ERROR "liburing not found: install with ./scripts/setup_liburing.sh")
    endif()
else()
    message(STATUS "Using system liburing: ${LIBURING_LIBRARIES}")
endif()

# ========== Sources ==========
set(SERVER_SOURCES
    main.cpp
    ui/mainwindow.cpp
    core/databasemanager.cpp
    core/servermanager.cpp
    core/businessmanager.cpp
    core/cframeworkadapter.cpp
    ui/memberwidget.cpp
    ui/goodswidget.cpp
    ui/orderwidget.cpp
    ui/clientwidget.cpp
    ui/otawidget.cpp
    ui/stylehelper.cpp
    ui/monitor_widget.cpp
    ui/video_player_widget.cpp
    core/stream_receiver.cpp
    core/thread_pool.c
    core/net_framework.c
    core/nf_epoll.c
    core/nf_iouring.c
)

set(SERVER_HEADERS
    ui/mainwindow.h
    core/databasemanager.h
    core/servermanager.h
    core/businessmanager.h
    core/cframeworkadapter.h
    core/thread_pool.h
    core/net_framework.h
    core/nf_epoll.h
    core/nf_iouring.h
    core/stream_receiver.h
    ui/memberwidget.h
    ui/goodswidget.h
    ui/orderwidget.h
    ui/clientwidget.h
    ui/otawidget.h
    ui/stylehelper.h
    ui/monitor_widget.h
    ui/video_player_widget.h
)

set(SERVER_UIS
    ui/mainwindow.ui
    ui/memberwidget.ui
    ui/goodswidget.ui
    ui/orderwidget.ui
    ui/clientwidget.ui
    ui/otawidget.ui
)

# ========== Common sources ==========
set(COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/crypto.c
)

set(COMMON_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson/cJSON.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/crypto.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/common.h
)

# ========== Build ==========
add_executable(${PROJECT_NAME}
    ${SERVER_SOURCES}
    ${SERVER_HEADERS}
    ${SERVER_UIS}
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson
    ${LIBURING_INCLUDE_DIRS}
)
if(FFMPEG_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    QT_DEPRECATED_WARNINGS
    # USE_IO_URING  # temporarily disabled: io_uring register buffers fails on this VM
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    Qt5::Sql
    OpenSSL::SSL
    OpenSSL::Crypto
    ${FFMPEG_LIBRARIES}
    pthread
    crypt
)
if(ALSA_LIBRARY)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ALSA_LIBRARY})
endif()

# liburing linkage
if(LIBURING_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${LIBURING_LIBRARY_DIRS})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "${LIBURING_RPATH}"
        BUILD_RPATH "${LIBURING_RPATH}"
    )
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBURING_LIBRARIES})
