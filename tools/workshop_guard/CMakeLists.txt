# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
cmake_minimum_required(VERSION 3.16)
project(workshop_guard LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "workshop_guard requires Windows x64")
endif()
add_executable(workshop-guard main.cpp)
target_compile_features(workshop-guard PRIVATE cxx_std_17)
target_link_libraries(workshop-guard PRIVATE bcrypt)
if(MINGW)
    target_link_options(workshop-guard PRIVATE -static)
endif()
