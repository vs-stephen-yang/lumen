// A1 link check (built only under ANDROID): proves the cross-compiled quiche
// static library + bundled BoringSSL link together with the POSIX socket and
// the portable core into an Android executable. Not run here — building it is
// the verification.

#include <quiche.h>

#include <cstdio>

namespace lumen {
bool AndroidCoreLinkCheck();  // platform/android/android_backend.cpp
}

int main() {
    std::printf("quiche %s\n", quiche_version());
    return lumen::AndroidCoreLinkCheck() ? 0 : 1;
}
