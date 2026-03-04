add_library(lumen_codec INTERFACE)
target_include_directories(lumen_codec INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(lumen_codec INTERFACE lumen_common)
