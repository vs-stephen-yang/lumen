# Android platform backend (Phase 2). Built only for ANDROID (NDK) configures.
#
# A0: links the portable core + the POSIX UDP socket so they compile under the
# NDK. Real capture/codec/render/transport backends are added in A3-A6.
add_library(lumen_platform_android STATIC
    android_backend.cpp
    codec/mediacodec_video_decoder.cpp   # A3: AMediaCodec video decode
    codec/mediacodec_video_encoder.cpp   # A3: AMediaCodec video encode
)

target_include_directories(lumen_platform_android PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lumen_platform_android PUBLIC
    lumen_common
    lumen_transport
    lumen_jitter_buffer
    lumen_codec       # Opus (libopus arm64-android, A2)
    lumen_platform_posix
    quiche            # cross-compiled for Android by the cargo bridge (A1)
    mediandk          # AMediaCodec / AMediaFormat (A3)
    android           # ANativeWindow
    log
)

# A1 link check: forces the full Android link chain — quiche + bundled
# BoringSSL + the POSIX socket + portable core — to link into an executable.
add_executable(android_quiche_linkcheck quiche_linkcheck.cpp)
target_link_libraries(android_quiche_linkcheck PRIVATE lumen_platform_android)

# A6: the JNI bridge shared library the Android app loads (liblumen_android.so).
add_library(lumen_android SHARED jni/lumen_jni.cpp)
target_link_libraries(lumen_android PRIVATE lumen_platform_android)
