add_library(lumen_renderer INTERFACE)
target_include_directories(lumen_renderer INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(lumen_renderer INTERFACE lumen_common)
