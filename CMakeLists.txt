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
