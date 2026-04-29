add_subdirectory(cpp-httplib)

# quiche — QUIC + HTTP/3 + BoringSSL (vendored as a Cargo workspace,
# bridged into CMake via the wrapper at third_party/quiche/CMakeLists.txt).
add_subdirectory(quiche)
