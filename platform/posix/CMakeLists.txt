# POSIX shared platform code (Android / macOS / iOS / Linux).
#
# Currently provides the BSD-sockets UdpSocket used by the quiche transport.
# Not wired into the root build yet — the per-platform subdirectories (Android
# first) will add_subdirectory() this and link lumen_platform_posix once their
# toolchains are in place.
add_library(lumen_platform_posix STATIC
    transport/udp_socket_posix.cpp
)

target_link_libraries(lumen_platform_posix PUBLIC
    lumen_transport   # UdpSocket interface lives in src/transport/include
)
