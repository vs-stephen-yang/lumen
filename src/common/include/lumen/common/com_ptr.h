#pragma once

// Minimal COM smart pointer for Windows. Avoids pulling in <wrl/client.h> so
// that non-Windows translation units can still include common headers.

#ifdef _WIN32

#include <wrl/client.h>

namespace lumen {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

}  // namespace lumen

#endif  // _WIN32
