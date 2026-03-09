if(WIN32)
    add_executable(capture_encode_test
        capture_encode_test.cpp
    )
    target_include_directories(capture_encode_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(capture_encode_test PRIVATE
        lumen_platform_windows
    )

    add_executable(encoder_test
        encoder_test.cpp
    )
    target_include_directories(encoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(encoder_test PRIVATE
        lumen_platform_windows
    )

    add_executable(video_adaptation_test
        video_adaptation_test.cpp
    )
    target_include_directories(video_adaptation_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(video_adaptation_test PRIVATE
        lumen_platform_windows
    )

    add_executable(decode_render_test
        decode_render_test.cpp
    )
    target_include_directories(decode_render_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    )
    target_link_libraries(decode_render_test PRIVATE
        lumen_platform_windows
    )

    add_executable(audio_capture_encode_test
        audio_capture_encode_test.cpp
    )
    target_include_directories(audio_capture_encode_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/platform/windows
        ${CMAKE_SOURCE_DIR}/src/codec
    )
    target_link_libraries(audio_capture_encode_test PRIVATE
        lumen_platform_windows
    )

    add_executable(audio_roundtrip_test
        audio_roundtrip_test.cpp
    )
    target_include_directories(audio_roundtrip_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/platform/windows
        ${CMAKE_SOURCE_DIR}/src/codec
    )
    target_link_libraries(audio_roundtrip_test PRIVATE
        lumen_platform_windows
    )
endif()
