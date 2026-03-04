add_library(lumen_capture INTERFACE)
target_include_directories(lumen_capture INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(lumen_capture INTERFACE lumen_common)
