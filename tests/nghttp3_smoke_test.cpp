// Smoke test: nghttp3 vendoring is wired correctly.
//
// Confirms the static library is reachable from this project's targets,
// the public header parses, and the library reports a sane version.

#include <nghttp3/nghttp3.h>

#include <cstdio>
#include <cstring>

int main() {
    const nghttp3_info* info = nghttp3_version(0);
    if (!info) {
        std::fprintf(stderr, "FAIL: nghttp3_version returned null\n");
        return 1;
    }
    std::printf("nghttp3 version: %s (api=%d)\n",
                info->version_str, info->version_num);
    if (info->version_str == nullptr || std::strlen(info->version_str) == 0) {
        std::fprintf(stderr, "FAIL: empty version string\n");
        return 1;
    }
    std::printf("nghttp3_smoke_test: ok\n");
    return 0;
}
