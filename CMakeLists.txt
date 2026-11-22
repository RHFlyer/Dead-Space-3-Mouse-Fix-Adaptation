# Builds the Raw Input Mouse Fix add-on for the DS3 Mod Loader:
#     cmake -S . -B build -A Win32
#     cmake --build build --config Release
# The mod is then ready to copy, as a folder, into the game's plugins folder: build\plugins\RawInputMouseFix\.
# scripts\package-mod.ps1 makes the release zip from it.

cmake_minimum_required(VERSION 3.20)
project(raw_mouse C CXX)

# Dead Space 3 is a 32-bit game, so mods must be 32-bit DLLs too. "-A Win32" picks that.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "deadspace3.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same build rules as the DS3 Mod Loader's SDK samples (its sdk/CMakeLists.txt, ds3_mod()). The DLL keeps the
# standalone fix's name, DS3RawMouse.asi, so an update replaces it in plugins\RawInputMouseFix\.
add_library(DS3RawMouse SHARED RawInputMouseFix/raw_mouse.cpp)
target_compile_definitions(DS3RawMouse PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(DS3RawMouse PRIVATE /W4 /permissive-)
endif()
# .asi like most game mods; the static C runtime, so players need no Visual C++ redistributable.
set_target_properties(DS3RawMouse PROPERTIES
    SUFFIX ".asi"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What ships: the DLL, mod.ini, CHANGELOG.txt and slopkhayzo's licence. Never the player's settings file
# (ds3_raw_mouse.ini): copying the folder over an older version would reset it.
set(out "${CMAKE_BINARY_DIR}/plugins/RawInputMouseFix")
add_custom_command(TARGET DS3RawMouse POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${out}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DS3RawMouse>"
            "${CMAKE_CURRENT_SOURCE_DIR}/RawInputMouseFix/mod.ini" "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/RawInputMouseFix/LICENSE-RawInputMouseFix.txt" "${out}"
    COMMENT "RawInputMouseFix: ready in ${out}")
