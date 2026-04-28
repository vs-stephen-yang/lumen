add_library(lumen_signaling STATIC
    src/signaling_server.cpp
)

target_include_directories(lumen_signaling PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lumen_signaling
    PUBLIC  lumen_common
    PRIVATE cpp_httplib
)
