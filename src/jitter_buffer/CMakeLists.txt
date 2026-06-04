add_library(lumen_jitter_buffer STATIC
    src/video_jitter_buffer.cpp
    src/audio_jitter_buffer.cpp
)

target_include_directories(lumen_jitter_buffer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Uses MediaPacketHeader from the transport types.
target_link_libraries(lumen_jitter_buffer PUBLIC
    lumen_common
    lumen_transport
)
