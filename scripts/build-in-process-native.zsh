#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
repo=${0:A:h:h}
: ${OPENXR_SOURCE_DIR:?Khronos OpenXR-SDK source checkout}
: ${MONADO_SOURCE_DIR:?macOS Monado source checkout}
: ${MWXR_MOLTENVK:?universal or x86_64 libMoltenVK.dylib}
root=${MWXR_NATIVE_BUILD:-${repo}/build-in-process/native}
cmake -S "${OPENXR_SOURCE_DIR}" -B "${root}/loader-x64" -G Ninja \
 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF \
 -DBUILD_LOADER=ON -DDYNAMIC_LOADER=ON -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
 -DCMAKE_CXX_FLAGS=-DJSONCPP_USING_SECURE_MEMORY=0
cmake --build "${root}/loader-x64" --target openxr_loader --parallel 6
# This target builds a runtime CLIENT, not a second x86_64 hardware service.
# MAIN is enabled to include the optional hosted compositor in the client.
cmake -S "${MONADO_SOURCE_DIR}" -B "${root}/monado-x64" -G Ninja \
 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DBUILD_TESTING=OFF \
 -DXRT_FEATURE_SERVICE=ON -DXRT_FEATURE_CLIENT_WITHOUT_SERVICE=OFF \
 -DXRT_HAVE_VULKAN=ON -DXRT_MODULE_COMPOSITOR_MAIN=ON \
 -DVulkan_LIBRARY="${MWXR_MOLTENVK}" -DXRT_MODULE_COMPOSITOR_NULL=OFF \
 -DXRT_FEATURE_OPENXR=ON -DXRT_BUILD_DRIVER_PSVR2=OFF -DXRT_BUILD_DRIVER_PSSENSE=OFF \
 -DXRT_FEATURE_DEBUG_GUI=OFF -DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
 -DXRT_BUILD_SAMPLES=OFF -DXRT_HAVE_OPENCV=OFF -DXRT_HAVE_SDL2=OFF \
 -DXRT_HAVE_JPEG=OFF -DXRT_FEATURE_OPENVR=OFF -DXRT_HAVE_OPENGL=OFF \
 -DXRT_HAVE_LIBUSB=OFF -DXRT_HAVE_HIDAPI=OFF -DXRT_BUILD_DRIVER_HANDTRACKING=OFF \
 -DXRT_BUILD_DRIVER_SIMULATED=ON -DXRT_FEATURE_WINDOW_PEEK=OFF
cmake --build "${root}/monado-x64" --target openxr_monado --parallel 6
print "Loader: ${root}/loader-x64/src/loader/libopenxr_loader.dylib"
print "Runtime manifest: ${root}/monado-x64/openxr_monado-dev.json"
