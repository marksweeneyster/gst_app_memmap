cmake_minimum_required(VERSION 3.16)

project(gst_shm_frame_broker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-app-1.0
  gstreamer-video-1.0)

find_package(Threads REQUIRED)

add_executable(gst_shm_writer
  src/gst_shm_writer.cpp)
target_link_libraries(gst_shm_writer
  PRIVATE PkgConfig::GST Threads::Threads)

add_executable(gst_shm_reader
  src/gst_shm_reader.cpp)
target_link_libraries(gst_shm_reader
  PRIVATE PkgConfig::GST Threads::Threads)
