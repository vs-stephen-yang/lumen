# Portable, dependency-free core — builds on every platform incl. Android.
add_subdirectory(common)
add_subdirectory(capture)        # interface-only
add_subdirectory(transport)
add_subdirectory(jitter_buffer)

# Modules with external dependencies not yet wired for Android:
#   codec     -> Opus            (Android: Phase 2 A2)
#   renderer  -> codec
#   signaling -> cpp-httplib     (third_party, host-only today)
if(NOT ANDROID)
    add_subdirectory(codec)
    add_subdirectory(renderer)
    add_subdirectory(signaling)
endif()
