add_library(lumen_transport STATIC
    src/media_packet_header.cpp
    src/frame_fragmenter.cpp
    src/frame_reassembler.cpp
)

target_include_directories(lumen_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lumen_transport PUBLIC lumen_common)
