# Cross-platform transport unit tests.
add_executable(transport_unit_test
    transport_unit_test.cpp
)
target_include_directories(transport_unit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
target_link_libraries(transport_unit_test PRIVATE
    lumen_transport
)

if(WIN32)
    # TCP transport integration test (Windows, Winsock2).
    add_executable(transport_tcp_test
        transport_tcp_test.cpp
    )
    target_include_directories(transport_tcp_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(transport_tcp_test PRIVATE
        lumen_platform_windows
    )

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

    add_executable(e2e_pipeline_test
        e2e_pipeline_test.cpp
    )
    target_include_directories(e2e_pipeline_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/platform/windows
        ${CMAKE_SOURCE_DIR}/src/codec
    )
    target_link_libraries(e2e_pipeline_test PRIVATE
        lumen_platform_windows
    )

    # QUIC transport integration test (requires msquic).
    if(LUMEN_HAS_QUIC)
        add_executable(transport_quic_test
            transport_quic_test.cpp
        )
        target_include_directories(transport_quic_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/mocks
            ${CMAKE_SOURCE_DIR}/platform/windows
        )
        target_link_libraries(transport_quic_test PRIVATE
            lumen_platform_windows
            crypt32
        )
    endif()
endif()
