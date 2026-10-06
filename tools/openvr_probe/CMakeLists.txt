# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# OpenVR probe application (openvr_probe.exe), cross-compiled with MinGW like
# the SteamVR driver:
#   cmake -S tools/openvr_probe -B build-in-process/openvr-probe -G Ninja \
#     -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
#     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
# It loads SteamVR's own openvr_api.dll at run time
# (scripts/run-openvr-probe.zsh copies it next to the executable).
cmake_minimum_required(VERSION 3.24)
project(mwxr_openvr_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(MINGW_DLLTOOL NAMES x86_64-w64-mingw32-dlltool dlltool REQUIRED)
set(openvr_import ${CMAKE_CURRENT_BINARY_DIR}/libopenvr_api.a)
add_custom_command(
	OUTPUT ${openvr_import}
	COMMAND ${MINGW_DLLTOOL} -d ${CMAKE_CURRENT_SOURCE_DIR}/openvr_api.def -l ${openvr_import}
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/openvr_api.def
	)
add_custom_target(openvr_api_import DEPENDS ${openvr_import})

add_executable(openvr_probe openvr_probe.cpp)
add_dependencies(openvr_probe openvr_api_import)
target_include_directories(openvr_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/steamvr_driver)
target_link_libraries(openvr_probe PRIVATE ${openvr_import} d3d11 d3dcompiler)
target_link_options(openvr_probe PRIVATE -static)
