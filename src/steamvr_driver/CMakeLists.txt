# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Win64 SteamVR driver (driver_mwxr.dll) that presents an OpenXR runtime's
# headset to SteamVR. Cross-compile with MinGW, for example:
#   cmake -S src/steamvr_driver -B build-steamvr-driver -G Ninja \
#     -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
#     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake_minimum_required(VERSION 3.24)
project(mwxr_steamvr_driver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# The Khronos loader, linked statically as in the hello_xr test build.
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(INSTALL_TO_ARCHITECTURE_PREFIXES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	openxr_sdk_source
	URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/211e2f4f0b79932300a845f3a8ed75026dbfa4a3.tar.gz
	URL_HASH SHA256=44747d4b90e0e4d5793da90ec7af9d38aa856b3c78d7fbdca0faad8029460325
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE
	)
FetchContent_MakeAvailable(openxr_sdk_source)
if(MINGW)
	# OpenXR-SDK 1.1.61 tests this Windows partition macro with -Werror=undef.
	target_compile_definitions(openxr_loader PRIVATE WINAPI_PARTITION_SYSTEM=0)
endif()

add_library(driver_mwxr SHARED driver.cpp xr_backend.cpp)
set_target_properties(driver_mwxr PROPERTIES PREFIX "")
target_include_directories(driver_mwxr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(driver_mwxr PRIVATE openxr_loader d3d11 dxgi)
if(MINGW)
	# No MinGW runtime DLLs beside vrserver.exe.
	target_link_options(driver_mwxr PRIVATE -static)
endif()

# SteamVR driver layout: <root>/driver.vrdrivermanifest, resources/, bin/win64/.
set(MWXR_DRIVER_ROOT "${CMAKE_BINARY_DIR}/mwxr")
add_custom_command(
	TARGET driver_mwxr
	POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${MWXR_DRIVER_ROOT}/bin/win64"
	COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:driver_mwxr>" "${MWXR_DRIVER_ROOT}/bin/win64/"
	COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest" "${MWXR_DRIVER_ROOT}/"
	COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${MWXR_DRIVER_ROOT}/resources"
	VERBATIM
	)
