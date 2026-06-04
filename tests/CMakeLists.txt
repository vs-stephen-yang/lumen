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

# Signaling server integration test.
add_executable(signaling_server_test
    signaling_server_test.cpp
)
target_link_libraries(signaling_server_test PRIVATE
    lumen_signaling
    cpp_httplib
)

# quiche FFI link smoke test (proves the cargo build + CMake bridge works).
add_executable(quiche_smoke_test
    quiche_smoke_test.cpp
)
target_link_libraries(quiche_smoke_test PRIVATE quiche)

# Cross-platform contract tests for the test-double mocks (header-only, no
# platform backends — runs on any OS).
add_executable(mock_contract_test
    mock_contract_test.cpp
)
target_include_directories(mock_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/codec/include
    ${CMAKE_SOURCE_DIR}/src/renderer/include
    ${CMAKE_SOURCE_DIR}/src/transport/include
)

# Cross-platform jitter buffer unit tests (clock injected — deterministic).
add_executable(jitter_buffer_test
    jitter_buffer_test.cpp
)
target_link_libraries(jitter_buffer_test PRIVATE
    lumen_jitter_buffer
)

if(WIN32)
    # Phase 2.1 — QuicheServer end-to-end QUIC handshake smoke test.
    add_executable(quiche_server_smoke_test
        quiche_server_smoke_test.cpp
    )
    target_include_directories(quiche_server_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(quiche_server_smoke_test PRIVATE
        lumen_platform_windows
    )

    # Phase 2.2 — HTTP/3 GET → 200 round-trip smoke test.
    add_executable(quiche_h3_smoke_test
        quiche_h3_smoke_test.cpp
    )
    target_include_directories(quiche_h3_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(quiche_h3_smoke_test PRIVATE
        lumen_platform_windows
    )

    # Phase 2.3 — extended CONNECT (WebTransport) smoke test.
    add_executable(quiche_wt_connect_smoke_test
        quiche_wt_connect_smoke_test.cpp
    )
    target_include_directories(quiche_wt_connect_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(quiche_wt_connect_smoke_test PRIVATE
        lumen_platform_windows
    )

    # Phase 2.4 — WT datagram round-trip smoke test.
    add_executable(quiche_wt_datagram_smoke_test
        quiche_wt_datagram_smoke_test.cpp
    )
    target_include_directories(quiche_wt_datagram_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(quiche_wt_datagram_smoke_test PRIVATE
        lumen_platform_windows
    )

    # Phase 2.5 — cert_util PEM export verified against quiche.
    add_executable(cert_util_pem_smoke_test
        cert_util_pem_smoke_test.cpp
    )
    target_include_directories(cert_util_pem_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(cert_util_pem_smoke_test PRIVATE
        lumen_platform_windows
    )

    # End-to-end: real web_receiver.exe + headless Chrome WebTransport.
    add_executable(web_e2e_test
        web_e2e_test.cpp
    )
    add_dependencies(web_e2e_test web_receiver)
endif()

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

    # WebSocket transport integration test.
    add_executable(ws_transport_test
        ws_transport_test.cpp
    )
    target_include_directories(ws_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(ws_transport_test PRIVATE
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

    # WebTransport adapter loopback test: a quiche client drives datagrams
    # through WebTransportServer and asserts the channel receive callbacks
    # deliver correctly reassembled [header][frame] data.
    add_executable(web_transport_adapter_test
        web_transport_adapter_test.cpp
    )
    target_include_directories(web_transport_adapter_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(web_transport_adapter_test PRIVATE
        lumen_platform_windows
        crypt32
    )

    # Native client <-> server loopback over the abstract interfaces (quiche),
    # restoring the native<->native coverage the old msquic test provided.
    add_executable(web_transport_client_loopback_test
        web_transport_client_loopback_test.cpp
    )
    target_include_directories(web_transport_client_loopback_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/platform/windows
    )
    target_link_libraries(web_transport_client_loopback_test PRIVATE
        lumen_platform_windows
        crypt32
    )
endif()
