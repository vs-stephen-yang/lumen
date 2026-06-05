# Portable, dependency-free core — builds on every platform incl. Android.
add_subdirectory(common)
add_subdirectory(capture)        # interface-only
add_subdirectory(transport)
add_subdirectory(jitter_buffer)
add_subdirectory(codec)          # Opus (Android: vcpkg arm64-android, A2)

# Still host-only:
#   renderer  -> codec + platform GPU (Android render is A5)
#   signaling -> cpp-httplib (third_party, host-only today)
if(NOT ANDROID)
    add_subdirectory(renderer)
    add_subdirectory(signaling)
endif()
