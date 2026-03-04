# Windows platform implementations for the Lumen pipeline.
# Links against D3D11, DXGI, and Media Foundation system libraries.

add_library(lumen_platform_windows STATIC
    common/d3d11_device_context.cpp
    capture/dxgi_desktop_duplication.cpp
    codec/d3d11_color_converter.cpp
    codec/mf_video_encoder.cpp
)

target_include_directories(lumen_platform_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lumen_platform_windows PUBLIC
    lumen_common
    lumen_capture
    lumen_codec
    d3d11
    dxgi
    mfplat
    mfuuid
    mf
)
