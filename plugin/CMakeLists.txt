cmake_minimum_required(VERSION 3.24)
project(SkyrimDelirium VERSION 0.1.0 LANGUAGES CXX)

# C++23 + dynamic CRT to match CommonLibSSE-NG (vcpkg triplet x64-windows-static-md).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# AE only (1.6.x). CommonLib checkout shared with SkyFluid; Delirium's C header from the Delirium checkout
# (the DLL is loaded at runtime, nothing links against it).
# Where they are: paths.local.json at the repo root (copy paths.example.json), else -D on the command line,
# else extern/ next to plugin/.
set(_commonlib "${CMAKE_CURRENT_LIST_DIR}/../extern/CommonLibSSE-NG")
set(_delirium "${CMAKE_CURRENT_LIST_DIR}/../extern/delirium-physics")
set(_paths "${CMAKE_CURRENT_LIST_DIR}/../paths.local.json")
if(EXISTS "${_paths}")
	file(READ "${_paths}" _json)
	string(JSON _v ERROR_VARIABLE _err GET "${_json}" commonlib)
	if(NOT _err)
		set(_commonlib "${_v}")
	endif()
	string(JSON _v ERROR_VARIABLE _err GET "${_json}" delirium)
	if(NOT _err)
		set(_delirium "${_v}")
	endif()
endif()
set(COMMONLIB_DIR "${_commonlib}" CACHE PATH "CommonLibSSE-NG checkout")
set(DELIRIUM_DIR "${_delirium}" CACHE PATH "Delirium checkout")
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
add_subdirectory(${COMMONLIB_DIR} commonlib EXCLUDE_FROM_ALL)

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

# host: SKSE entry, hooks, keys, command file. core: the engine, loaded by the host from a temporary copy so it
# can be rebuilt and reloaded while the game runs (tools\hotreload.ps1).
add_library(SkyrimDelirium SHARED src/host/main.cpp)
add_library(SkyrimDeliriumCore SHARED
	src/core/core.cpp src/core/engine.cpp src/core/hooks.cpp src/core/settings/table.cpp src/core/settings/load.cpp
	src/core/settings/live.cpp src/core/settings/save.cpp src/core/menu/menu.cpp src/core/menu/reactions.cpp
	src/core/reactions/hits.cpp src/core/reactions/rules.cpp src/core/reactions/react.cpp src/core/reactions/dying.cpp
	src/core/body/takeover.cpp src/core/body/update.cpp src/core/body/calibrate.cpp src/core/body/drive.cpp src/core/body/skyrim_body.cpp
	src/core/body/collision.cpp src/core/body/show.cpp src/core/body/fit.cpp
	src/core/debug/commands.cpp src/core/debug/trace.cpp src/core/debug/settings_dump.cpp)
# offline settings check (tools\check-settings.ps1): built only when asked for
add_executable(SettingsCheck EXCLUDE_FROM_ALL tests/settings_check.cpp
	src/core/settings/table.cpp src/core/settings/load.cpp src/core/settings/live.cpp src/core/settings/save.cpp
	src/core/reactions/rules.cpp src/core/debug/settings_dump.cpp)
foreach(t SkyrimDelirium SkyrimDeliriumCore SettingsCheck)
	target_include_directories(${t} PRIVATE src ${SIMPLEINI_INCLUDE_DIRS} "${DELIRIUM_DIR}/crates/delirium-sdk/include")
	target_precompile_headers(${t} PRIVATE src/PCH.h)
	target_link_libraries(${t} PRIVATE CommonLibSSE::CommonLibSSE)
	target_compile_definitions(${t} PRIVATE DELIRIUM_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
	target_compile_options(${t} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /wd4100 /wd4324)
	# the DLL names its PDB without a folder: debuggers find it beside the loaded copy, never lock the build's
	target_link_options(${t} PRIVATE /DEBUG:FULL /PDBALTPATH:%_PDB%)
endforeach()
# the core's files include "shared.h", "engine.h" from any of its folders
target_include_directories(SkyrimDeliriumCore PRIVATE src/core)
target_include_directories(SettingsCheck PRIVATE src/core)
