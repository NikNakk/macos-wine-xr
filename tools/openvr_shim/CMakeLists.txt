# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Stand-in openvr_api.dll that marks SteamVR's "uninitialised" HMD render
# pose valid under Wine (see openvr_shim.cpp), cross-compiled with MinGW:
#   cmake -S tools/openvr_shim -B build-in-process/openvr-shim -G Ninja \
#     -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
#     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake_minimum_required(VERSION 3.24)
project(mwxr_openvr_shim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(openvr_api SHARED openvr_shim.cpp openvr_api.def)
set_target_properties(openvr_api PROPERTIES PREFIX "" OUTPUT_NAME openvr_api)
target_include_directories(openvr_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/steamvr_driver)
target_link_options(openvr_api PRIVATE -static)
