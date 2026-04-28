# Header-only target wrapping the vendored cpp-httplib.
# Source: https://github.com/yhirose/cpp-httplib v0.18.7

add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
