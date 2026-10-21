cmake_minimum_required(VERSION 3.16)

# Keep the target reproducible even when invoked from a fresh IDF shell.
set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
set(PROJECT_VER "1.0.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(s3_serial_debugger)
