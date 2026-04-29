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
set(_quiche_target    ${_quiche_root}/target/${_quiche_profile_dir})
set(_quiche_lib_path  ${_quiche_target}/quiche.lib)
set(_quiche_inc_path  ${_quiche_root}/quiche/include)
set(_quiche_manifest  ${_quiche_root}/quiche/Cargo.toml)

# A stamp file that custom_command updates on every run; makes
# downstream targets see "quiche.lib was rebuilt".
set(_quiche_stamp ${CMAKE_CURRENT_BINARY_DIR}/quiche.stamp)

add_custom_command(
    OUTPUT  ${_quiche_lib_path} ${_quiche_stamp}
    COMMAND ${CARGO_EXECUTABLE} build
            --profile ${_quiche_profile}
            --features ffi
            --manifest-path ${_quiche_manifest}
    COMMAND ${CMAKE_COMMAND} -E touch ${_quiche_stamp}
    WORKING_DIRECTORY ${_quiche_root}
    COMMENT "Building quiche (cargo --profile ${_quiche_profile})"
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
