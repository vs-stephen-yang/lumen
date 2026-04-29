# Windows platform implementations for the Lumen pipeline.
# Links against D3D11, DXGI, and Media Foundation system libraries.

set(LUMEN_PLATFORM_WIN_SOURCES
    common/d3d11_device_context.cpp
    common/cert_util.cpp
    capture/dxgi_desktop_duplication.cpp
    capture/wasapi_loopback_capture.cpp
    codec/d3d11_color_converter.cpp
    codec/mf_video_encoder.cpp
    codec/mf_video_decoder.cpp
    renderer/d3d11_swapchain_renderer.cpp
    renderer/wasapi_audio_renderer.cpp
    transport/tcp_transport_channel.cpp
    transport/tcp_transport_connection.cpp
    transport/tcp_transport_client.cpp
    transport/tcp_transport_server.cpp
    transport/ws_frame_codec.cpp
    transport/ws_handshake.cpp
    transport/ws_transport_channel.cpp
    transport/ws_transport_connection.cpp
    transport/ws_transport_server.cpp
    transport/quiche_server.cpp
    transport/wt_varint.cpp
)

# Native-only QUIC transport (msquic, Schannel TLS). Used for native↔native
# scenarios; the browser-facing WebTransport path uses quiche, which is
# wired in separately.
find_package(msquic CONFIG QUIET)
if(msquic_FOUND)
    list(APPEND LUMEN_PLATFORM_WIN_SOURCES
        transport/quic_channel.cpp
        transport/quic_transport_connection.cpp
        transport/quic_transport_client.cpp
        transport/quic_transport_server.cpp
    )
    set(LUMEN_HAS_QUIC ON CACHE INTERNAL "")
    message(STATUS "msquic found — building native QUIC transport backend")
else()
    set(LUMEN_HAS_QUIC OFF CACHE INTERNAL "")
    message(STATUS "msquic not found — skipping native QUIC transport")
endif()

add_library(lumen_platform_windows STATIC ${LUMEN_PLATFORM_WIN_SOURCES})

target_include_directories(lumen_platform_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lumen_platform_windows PUBLIC
    lumen_common
    lumen_capture
    lumen_codec
    lumen_transport
    lumen_renderer
    d3d11
    dxgi
    mfplat
    mfuuid
    mf
    ole32
    avrt
    ws2_32
    bcrypt
)

target_link_libraries(lumen_platform_windows
    PUBLIC quiche
    PRIVATE crypt32 ncrypt
)

if(LUMEN_HAS_QUIC)
    target_link_libraries(lumen_platform_windows PUBLIC msquic ntdll)
    target_compile_definitions(lumen_platform_windows PUBLIC LUMEN_HAS_QUIC=1)
endif()

add_subdirectory(apps/web_receiver)
