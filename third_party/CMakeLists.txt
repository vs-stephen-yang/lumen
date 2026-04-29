add_subdirectory(cpp-httplib)

# nghttp3 — HTTP/3 framing + QPACK. Vendored at v1.15.0.
# We only want the library (no examples, no tests, no shared lib).
set(ENABLE_LIB_ONLY   ON  CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB ON  CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
add_subdirectory(nghttp3)

# nghttp3_static only sets the INSTALL_INTERFACE include dir; we use it
# in-tree so add the BUILD_INTERFACE dirs for consumers.
target_include_directories(nghttp3_static INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/nghttp3/lib/includes>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/nghttp3/lib/includes>
)
