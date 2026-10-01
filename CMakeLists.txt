cmake_minimum_required(VERSION 3.21)
project(macos_wine_xr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DXMT_INCLUDE_DIR "" CACHE PATH "Path to the DXMT include directory containing dxmt_native_interop.h")

if(WIN32)
	if(NOT DXMT_INCLUDE_DIR)
		message(FATAL_ERROR "Set DXMT_INCLUDE_DIR to the include directory from NikNakk/dxmt:macos-xr-native-sharing")
	endif()

	add_library(macos_wine_xr_dxmt STATIC
		src/windows/dxmt_native_sharing.cpp
	)

	target_include_directories(macos_wine_xr_dxmt
		PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/include
		PRIVATE
			${DXMT_INCLUDE_DIR}
	)

	target_link_libraries(macos_wine_xr_dxmt PRIVATE d3d11 dxguid)

	add_executable(dxmt_native_sharing_probe
		tests/windows/dxmt_native_sharing_probe.cpp
	)
	target_link_libraries(dxmt_native_sharing_probe
		PRIVATE macos_wine_xr_dxmt d3d11 dxgi dxguid
	)
endif()

if(APPLE)
	enable_language(OBJC)

	add_library(macos_wine_xr_native_metal STATIC
		src/macos/native_metal_sharing.m
		src/macos/monado_handoff.c
	)
	target_include_directories(macos_wine_xr_native_metal
		PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/include
	)
	target_link_libraries(macos_wine_xr_native_metal
		PRIVATE
			"-framework Foundation"
			"-framework Metal"
			${CMAKE_DL_LIBS}
	)

	add_executable(native_metal_sharing_probe
		tests/macos/native_metal_sharing_probe.m
	)
	target_link_libraries(native_metal_sharing_probe
		PRIVATE
			macos_wine_xr_native_metal
			"-framework Foundation"
			"-framework Metal"
	)

	add_executable(dxmt_to_monado_token_probe
		tests/macos/dxmt_to_monado_token_probe.m
	)
	target_link_libraries(dxmt_to_monado_token_probe
		PRIVATE
			macos_wine_xr_native_metal
			"-framework Foundation"
			"-framework Metal"
	)
endif()
