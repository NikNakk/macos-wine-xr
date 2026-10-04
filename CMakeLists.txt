cmake_minimum_required(VERSION 3.21)
project(macos_wine_xr LANGUAGES C CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DXMT_INCLUDE_DIR "" CACHE PATH "Path to the DXMT include directory containing dxmt_native_interop.h")
set(MONADO_SOURCE_DIR "" CACHE PATH "Path to NikNakk/monado source (macos-upstream-clean)")
set(MONADO_BUILD_DIR "" CACHE PATH "Matching configured Monado build directory")
set(OPENXR_INCLUDE_DIR "" CACHE PATH "Khronos OpenXR include directory")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MWXR_RPC_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/openxr_rpc_generated.h")
add_custom_command(OUTPUT "${MWXR_RPC_GENERATED}"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_openxr_rpc.py"
 --schema "${CMAKE_CURRENT_SOURCE_DIR}/protocol/openxr.json" --output "${MWXR_RPC_GENERATED}"
 DEPENDS tools/generate_openxr_rpc.py protocol/openxr.json VERBATIM)
add_library(macos_wine_xr_rpc STATIC src/rpc/transport.c src/proxy/tcp_auth.c "${MWXR_RPC_GENERATED}")
target_include_directories(macos_wine_xr_rpc PUBLIC src/rpc "${CMAKE_CURRENT_BINARY_DIR}/generated" PRIVATE src/proxy)
add_executable(rpc_serialization_test tests/rpc/serialization_test.c "${MWXR_RPC_GENERATED}")
 target_include_directories(rpc_serialization_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
 if(NOT WIN32)
 add_test(NAME rpc_serialization COMMAND rpc_serialization_test)
 endif()
add_executable(fence_wait_test tests/in_process/fence_wait_test.cpp)
add_test(NAME fence_wait COMMAND fence_wait_test)

if(WIN32)
 target_link_libraries(macos_wine_xr_rpc PUBLIC ws2_32 bcrypt)
endif()

if(WIN32)
	if(NOT DXMT_INCLUDE_DIR)
		message(FATAL_ERROR "Set DXMT_INCLUDE_DIR to the include directory from NikNakk/dxmt:macos-xr-native-sharing")
	endif()

	if(OPENXR_INCLUDE_DIR)
 add_executable(openxr_rpc_smoke tests/windows/openxr_rpc_smoke.cpp)
 target_include_directories(openxr_rpc_smoke PRIVATE "${OPENXR_INCLUDE_DIR}")
 target_link_libraries(openxr_rpc_smoke PRIVATE d3d11 dxguid)
 if(MINGW)
 target_link_options(openxr_rpc_smoke PRIVATE -static -static-libgcc -static-libstdc++)
 endif()
 add_library(macos_wine_xr_openxr SHARED src/windows/openxr_thunk.cpp)
 target_include_directories(macos_wine_xr_openxr PRIVATE "${OPENXR_INCLUDE_DIR}" "${DXMT_INCLUDE_DIR}")
 target_link_libraries(macos_wine_xr_openxr PRIVATE macos_wine_xr_rpc d3d11 dxgi dxguid)
 set_target_properties(macos_wine_xr_openxr PROPERTIES PREFIX "")
 if(MINGW)
  target_link_options(macos_wine_xr_openxr PRIVATE -static -static-libgcc -static-libstdc++)
 endif()
 file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/macos_wine_xr_openxr.json" CONTENT "{\"file_format_version\":\"1.0.0\",\"runtime\":{\"library_path\":\"./macos_wine_xr_openxr.dll\"}}\n")
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

	add_executable(dxmt_window_probe tests/windows/dxmt_window_probe.cpp)
	target_link_libraries(dxmt_window_probe PRIVATE d3d11 dxgi dxguid)
	if(MINGW)
		target_link_options(dxmt_window_probe PRIVATE -static -static-libgcc -static-libstdc++)
	endif()

	add_executable(dxmt_native_sharing_probe
		tests/windows/dxmt_native_sharing_probe.cpp
	)
	target_link_libraries(dxmt_native_sharing_probe
		PRIVATE macos_wine_xr_dxmt d3d11 dxgi dxguid
	)
	add_executable(dxmt_reverse_import_probe tests/windows/dxmt_reverse_import_probe.cpp)
	target_include_directories(dxmt_reverse_import_probe PRIVATE "${DXMT_INCLUDE_DIR}")
	target_link_libraries(dxmt_reverse_import_probe PRIVATE macos_wine_xr_dxmt d3d11 dxgi dxguid)
	if(MINGW)
		target_link_options(dxmt_reverse_import_probe PRIVATE -static -static-libgcc -static-libstdc++)
	endif()
endif()

if(APPLE)
	enable_language(OBJC)
 add_executable(rpc_transport_test tests/rpc/transport_test.c)
 target_link_libraries(rpc_transport_test PRIVATE macos_wine_xr_rpc)
 add_test(NAME rpc_transport COMMAND rpc_transport_test)
 set_tests_properties(rpc_transport PROPERTIES TIMEOUT 10)
	add_executable(graphics_interposer_control_test tests/macos/graphics_interposer_control_test.m)
	target_link_libraries(graphics_interposer_control_test PRIVATE "-framework Metal" "-framework Foundation")
	add_test(NAME graphics_interposer_control COMMAND graphics_interposer_control_test)
	set_tests_properties(graphics_interposer_control PROPERTIES TIMEOUT 10)
	# In-process graphics interop allocation interposer; needs only Metal.
	add_executable(graphics_interposer_test
		tests/macos/graphics_interposer_test.m src/in_process/graphics_interop_native.m)
	target_link_libraries(graphics_interposer_test PRIVATE
		"-framework Metal" "-framework Foundation" "-framework IOSurface")
	add_test(NAME graphics_interposer COMMAND graphics_interposer_test)
	set_tests_properties(graphics_interposer PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
	if(NOT OPENXR_INCLUDE_DIR)
		find_path(MWXR_DISCOVERED_OPENXR_INCLUDE_DIR openxr/openxr.h)
		if(MWXR_DISCOVERED_OPENXR_INCLUDE_DIR)
			set(OPENXR_INCLUDE_DIR "${MWXR_DISCOVERED_OPENXR_INCLUDE_DIR}")
		endif()
	endif()
	if(OPENXR_INCLUDE_DIR)
		add_library(macos_wine_xr_native_openxr STATIC
			src/native/native_openxr_backend.m src/native/openxr_swapchain.m)
		target_include_directories(macos_wine_xr_native_openxr PUBLIC include "${OPENXR_INCLUDE_DIR}")
		target_link_libraries(macos_wine_xr_native_openxr PRIVATE
			"-framework Foundation" "-framework Metal" ${CMAKE_DL_LIBS})
		if(DXMT_INCLUDE_DIR)
 add_executable(macos_wine_xr_host src/host/host.m)
 target_include_directories(macos_wine_xr_host PRIVATE src/native src/proxy)
 target_link_libraries(macos_wine_xr_host PRIVATE macos_wine_xr_native_openxr macos_wine_xr_rpc macos_wine_xr_native_capability)
endif()
add_executable(native_openxr_backend_probe tests/macos/native_openxr_backend_probe.c)
		target_link_libraries(native_openxr_backend_probe PRIVATE macos_wine_xr_native_openxr)
		add_executable(native_openxr_blit_probe tests/macos/native_openxr_blit_probe.m)
		target_link_libraries(native_openxr_blit_probe PRIVATE macos_wine_xr_native_openxr)
	else()
		message(STATUS "OpenXR headers not found; native OpenXR backend disabled")
	endif()

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
	if(DXMT_INCLUDE_DIR)
		add_library(macos_wine_xr_native_capability STATIC src/macos/native_capability.m)
		target_include_directories(macos_wine_xr_native_capability PUBLIC include PRIVATE "${DXMT_INCLUDE_DIR}")
		target_link_libraries(macos_wine_xr_native_capability PRIVATE "-framework Foundation" bsm)
		add_executable(dxmt_reverse_import_probe tests/macos/dxmt_reverse_import_probe.m)
		target_link_libraries(dxmt_reverse_import_probe PRIVATE macos_wine_xr_native_metal macos_wine_xr_native_capability
			"-framework Foundation" "-framework Metal")
		if(TARGET macos_wine_xr_native_openxr)
			target_compile_definitions(dxmt_reverse_import_probe PRIVATE MWXR_HAVE_NATIVE_OPENXR=1)
			target_link_libraries(dxmt_reverse_import_probe PRIVATE macos_wine_xr_native_openxr)
		endif()
		add_executable(native_capability_test tests/macos/native_capability_test.c)
		target_include_directories(native_capability_test PRIVATE "${DXMT_INCLUDE_DIR}")
		target_link_libraries(native_capability_test PRIVATE macos_wine_xr_native_capability)
		add_test(NAME native_capability COMMAND native_capability_test)
	endif()
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

	add_executable(tcp_auth_test
		tests/macos/tcp_auth_test.c
		src/proxy/tcp_auth.c
	)
	target_include_directories(tcp_auth_test PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src/proxy
	)
	add_test(NAME tcp_auth COMMAND tcp_auth_test)

	if(MONADO_SOURCE_DIR AND MONADO_BUILD_DIR)
		find_package(Python3 REQUIRED COMPONENTS Interpreter)
		file(GLOB MONADO_PROTO_JSON CONFIGURE_DEPENDS
			"${MONADO_SOURCE_DIR}/src/xrt/ipc/shared/proto/*.json"
		)
		set(MONADO_WIRE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
		set(MONADO_WIRE_GENERATED_HEADER
			"${MONADO_WIRE_GENERATED_DIR}/monado_wire_generated.h"
		)
		add_custom_command(
			OUTPUT "${MONADO_WIRE_GENERATED_HEADER}"
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_monado_wire.py"
				--monado-source "${MONADO_SOURCE_DIR}"
				--generated-header "${MONADO_BUILD_DIR}/src/xrt/ipc/ipc_protocol_generated.h"
				--output "${MONADO_WIRE_GENERATED_HEADER}"
			DEPENDS
				"${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_monado_wire.py"
				"${MONADO_BUILD_DIR}/src/xrt/ipc/ipc_protocol_generated.h"
				${MONADO_PROTO_JSON}
			VERBATIM
			COMMENT "Generating Monado IPC wire description for Wine proxy"
		)

		add_executable(macos_wine_xr_proxy
			src/proxy/monado_proxy.m
			src/proxy/tcp_auth.c
			src/proxy/tcp_auth.h
			"${MONADO_WIRE_GENERATED_HEADER}"
		)
		target_include_directories(macos_wine_xr_proxy
			PRIVATE
				"${CMAKE_CURRENT_SOURCE_DIR}/include"
				"${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
				"${MONADO_WIRE_GENERATED_DIR}"
				"${MONADO_SOURCE_DIR}/src/xrt/include"
				"${MONADO_BUILD_DIR}/src/xrt/include"
				"${MONADO_SOURCE_DIR}/src/xrt/ipc"
				"${MONADO_BUILD_DIR}/src/xrt/ipc"
		)
		target_link_libraries(macos_wine_xr_proxy
			PRIVATE
				macos_wine_xr_native_metal
				"-framework Foundation"
				"-framework Metal"
		)
	else()
		message(STATUS "MONADO_SOURCE_DIR/MONADO_BUILD_DIR not set; standalone Wine proxy will not be built")
	endif()
endif()
