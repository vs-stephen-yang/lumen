find_package(Opus CONFIG REQUIRED)

add_library(lumen_codec STATIC
    opus/opus_audio_encoder.cpp
    opus/opus_audio_decoder.cpp
)

target_include_directories(lumen_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lumen_codec PUBLIC
    lumen_common
    Opus::opus
)
