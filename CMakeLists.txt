cmake_minimum_required(VERSION 3.20)
project(lumen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Force MD (release CRT) on every config. Rust's quiche.lib always links
# to the release CRT and there's no "debug CRT" concept on the Rust side,
# so the Debug build of our C++ has to follow suit. Without this, the
# MSVC linker hits LNK2038 on _ITERATOR_DEBUG_LEVEL when any test or app
# links lumen_platform_windows (which transitively links quiche).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        CACHE STRING "Forced /MD for Rust-staticlib compatibility" FORCE)
endif()

# third_party (quiche/cargo, cpp-httplib) is host/Windows-oriented today.
# The Android core build (Phase 2 A0) skips it; quiche-for-Android is A1.
if(NOT ANDROID)
    add_subdirectory(third_party)
endif()

add_subdirectory(src)

if(WIN32)
    add_subdirectory(platform/windows)
endif()

if(ANDROID)
    add_subdirectory(platform/posix)
    add_subdirectory(platform/android)
endif()

# Host test executables — not built for Android (on-device tests come in A7).
if(NOT ANDROID)
    add_subdirectory(tests)
endif()
