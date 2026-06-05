# CMake bridge for vendored quiche.
#
# This file is OUR wrapper, NOT the upstream's. Upstream quiche is built
# via cargo; we run cargo from CMake via a custom command and expose the
# resulting static library as a CMake imported target named `quiche`.
#
# Layout assumptions:
#   third_party/quiche/quiche/Cargo.toml             (upstream crate)
#   third_party/quiche/quiche/include/quiche.h        (FFI header)
#   third_party/quiche/target/<profile>/quiche.lib    (cargo output)

find_program(CARGO_EXECUTABLE cargo
    HINTS $ENV{USERPROFILE}/.cargo/bin $ENV{HOME}/.cargo/bin
    REQUIRED
)
message(STATUS "quiche: cargo at ${CARGO_EXECUTABLE}")

# Match the cargo profile to CMake's. cargo's two built-in profiles are
# `dev` (target/debug) and `release` (target/release); higher CMake build
# types are best mapped to release since dev is slow and unoptimized.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_quiche_profile      "dev")
    set(_quiche_profile_dir  "debug")
else()
    set(_quiche_profile      "release")
    set(_quiche_profile_dir  "release")
endif()

set(_quiche_root      ${CMAKE_CURRENT_SOURCE_DIR})
set(_quiche_inc_path  ${_quiche_root}/quiche/include)
set(_quiche_manifest  ${_quiche_root}/quiche/Cargo.toml)

if(ANDROID)
    # Map the NDK ABI to a Rust target triple + clang wrapper prefix.
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_rust_triple aarch64-linux-android)
        set(_clang_prefix aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_rust_triple armv7-linux-androideabi)
        set(_clang_prefix armv7a-linux-androideabi)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_rust_triple x86_64-linux-android)
        set(_clang_prefix x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(_rust_triple i686-linux-android)
        set(_clang_prefix i686-linux-android)
    else()
        message(FATAL_ERROR "quiche: unsupported ANDROID_ABI '${ANDROID_ABI}'")
    endif()

    # Upper-case underscored triple for the CARGO_TARGET_<T>_LINKER env key.
    string(TOUPPER "${_rust_triple}" _tr_uc)
    string(REPLACE "-" "_" _tr_uc "${_tr_uc}")

    set(_ndk_root ${CMAKE_ANDROID_NDK})
    if(NOT _ndk_root)
        set(_ndk_root $ENV{ANDROID_NDK_HOME})
    endif()

    # Numeric API level for the NDK clang wrapper name (…androidNN-clang).
    # CMAKE_SYSTEM_VERSION is unreliable across NDK toolchain versions.
    if(DEFINED ANDROID_PLATFORM_LEVEL)
        set(_api ${ANDROID_PLATFORM_LEVEL})
    elseif(ANDROID_PLATFORM MATCHES "android-([0-9]+)")
        set(_api ${CMAKE_MATCH_1})
    else()
        set(_api 29)
    endif()

    get_filename_component(_ninja_dir "${CMAKE_MAKE_PROGRAM}" DIRECTORY)

    set(_quiche_lib_path
        ${_quiche_root}/target/${_rust_triple}/${_quiche_profile_dir}/libquiche.a)

    # Windows-host wrapper sets the NDK toolchain env + Ninja generator and
    # runs the cross cargo build (see build_quiche_android.cmd).
    set(_quiche_build_cmd
        cmd /c ${_quiche_root}/build_quiche_android.cmd
        "${_ndk_root}" "${_rust_triple}" "${_clang_prefix}"
        "${_api}" "${_quiche_profile}" "${_quiche_manifest}"
        "${CARGO_EXECUTABLE}" "${_ninja_dir}" "${_tr_uc}")
    set(_quiche_comment "Building quiche for Android (${_rust_triple})")
else()
    set(_quiche_lib_path ${_quiche_root}/target/${_quiche_profile_dir}/quiche.lib)
    set(_quiche_build_cmd ${CARGO_EXECUTABLE} build
        --profile ${_quiche_profile} --features ffi
        --manifest-path ${_quiche_manifest})
    set(_quiche_comment "Building quiche (cargo --profile ${_quiche_profile})")
endif()

# A stamp file that custom_command updates on every run; makes
# downstream targets see "the quiche lib was rebuilt".
set(_quiche_stamp ${CMAKE_CURRENT_BINARY_DIR}/quiche.stamp)

add_custom_command(
    OUTPUT  ${_quiche_lib_path} ${_quiche_stamp}
    COMMAND ${_quiche_build_cmd}
    COMMAND ${CMAKE_COMMAND} -E touch ${_quiche_stamp}
    WORKING_DIRECTORY ${_quiche_root}
    COMMENT "${_quiche_comment}"
    VERBATIM
)

add_custom_target(quiche_build DEPENDS ${_quiche_lib_path})

# Imported static library. Consumers do `target_link_libraries(t PRIVATE quiche)`.
add_library(quiche STATIC IMPORTED GLOBAL)
add_dependencies(quiche quiche_build)
set_target_properties(quiche PROPERTIES
    IMPORTED_LOCATION ${_quiche_lib_path}
    INTERFACE_INCLUDE_DIRECTORIES ${_quiche_inc_path}
)

# Rust staticlibs on Windows-MSVC pull in these system libs. Without
# them the linker complains about unresolved __imp_* symbols from the
# Rust stdlib and BoringSSL.
if(WIN32)
    target_link_libraries(quiche INTERFACE
        ws2_32
        userenv
        ntdll
        bcrypt
        advapi32
        crypt32
        iphlpapi
        legacy_stdio_definitions
    )
endif()

# Android: the Rust staticlib pulls in std (android_log) and BoringSSL needs
# the dynamic loader / math libs.
if(ANDROID)
    target_link_libraries(quiche INTERFACE log dl m)
endif()
