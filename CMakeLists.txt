cmake_minimum_required(VERSION 3.14)
project(gst-uav-videolink C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST      REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GLIB     REQUIRED glib-2.0)
pkg_check_modules(GST_RTSP      REQUIRED gstreamer-rtsp-server-1.0>=1.16)
pkg_check_modules(GST_RTSP_LIB REQUIRED gstreamer-rtsp-1.0>=1.16)

# MAVLink C library v2 — expected as a git submodule at third_party/mavlink/
# git submodule add https://github.com/mavlink/c_library_v2.git third_party/mavlink
set(MAVLINK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party")
if(NOT EXISTS "${MAVLINK_INCLUDE_DIR}/mavlink/common/mavlink.h")
    message(FATAL_ERROR
        "MAVLink headers not found at ${MAVLINK_INCLUDE_DIR}/mavlink/\n"
        "Run: git submodule add https://github.com/mavlink/c_library_v2.git third_party/mavlink\n"
        "     git submodule update --init")
endif()

# mavlink_announce is compiled once and shared between sender and rtsp_server.
# It has no GStreamer dependency — only GLib (for gboolean/g_print) and POSIX sockets.
add_library(mavlink_announce STATIC src/mavlink_announce.c)
target_include_directories(mavlink_announce PUBLIC
    inc
    ${MAVLINK_INCLUDE_DIR}
    ${GLIB_INCLUDE_DIRS})
target_compile_options(mavlink_announce PRIVATE ${GLIB_CFLAGS_OTHER})
target_link_libraries  (mavlink_announce PRIVATE ${GLIB_LIBRARIES})

# ── sender ────────────────────────────────────────────────────────────────────
add_executable(sender sender.c)
target_include_directories(sender PRIVATE ${GST_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS} inc)
target_link_directories   (sender PRIVATE ${GST_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS})
target_compile_options    (sender PRIVATE ${GST_CFLAGS_OTHER}  ${GLIB_CFLAGS_OTHER})
target_link_libraries     (sender PRIVATE ${GST_LIBRARIES} ${GLIB_LIBRARIES} mavlink_announce)

# ── receiver ──────────────────────────────────────────────────────────────────
add_executable(receiver receiver.c)
target_include_directories(receiver PRIVATE ${GST_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
target_link_directories   (receiver PRIVATE ${GST_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS})
target_compile_options    (receiver PRIVATE ${GST_CFLAGS_OTHER}  ${GLIB_CFLAGS_OTHER})
target_link_libraries     (receiver PRIVATE ${GST_LIBRARIES}     ${GLIB_LIBRARIES})

# ── rtsp_server ───────────────────────────────────────────────────────────────
add_executable(rtsp_server rtsp_server.c)
target_include_directories(rtsp_server PRIVATE
    ${GST_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS} ${GST_RTSP_INCLUDE_DIRS} ${GST_RTSP_LIB_INCLUDE_DIRS} inc)
target_link_directories   (rtsp_server PRIVATE
    ${GST_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS} ${GST_RTSP_LIBRARY_DIRS} ${GST_RTSP_LIB_LIBRARY_DIRS})
target_compile_options    (rtsp_server PRIVATE
    ${GST_CFLAGS_OTHER} ${GLIB_CFLAGS_OTHER} ${GST_RTSP_CFLAGS_OTHER} ${GST_RTSP_LIB_CFLAGS_OTHER})
target_link_libraries     (rtsp_server PRIVATE
    ${GST_LIBRARIES} ${GLIB_LIBRARIES} ${GST_RTSP_LIBRARIES} ${GST_RTSP_LIB_LIBRARIES} mavlink_announce)
