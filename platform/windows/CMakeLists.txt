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
    transport/web_transport_channel.cpp
    transport/web_transport_connection.cpp
    transport/web_transport_server.cpp
)

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

add_subdirectory(apps/web_receiver)
