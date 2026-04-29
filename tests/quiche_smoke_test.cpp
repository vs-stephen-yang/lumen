// Smoke test: vendored quiche (Cargo + BoringSSL) is reachable via FFI.
//
// Confirms the bridge in third_party/quiche/CMakeLists.txt produces a
// usable static library, and that calling quiche_version() returns a
// non-empty version string.

#include <quiche.h>

#include <cstdio>
#include <cstring>

int main() {
    const char* v = quiche_version();
    if (!v || std::strlen(v) == 0) {
        std::fprintf(stderr, "FAIL: quiche_version() returned %s\n",
                      v ? "empty string" : "null");
        return 1;
    }
    std::printf("quiche version: %s\n", v);
    std::printf("quiche_smoke_test: ok\n");
    return 0;
}
