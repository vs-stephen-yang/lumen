# Flutter Windows Zero-Copy Decode & Render Test App

## Context
The Lumen project has a working Windows zero-copy decode→render pipeline (MfVideoDecoder → D3D11ColorConverter → D3D11SwapChainRenderer) tested via a standalone C++ test (`decode_render_test.cpp`). The project is designed to integrate into Flutter via FFI/platform channels (per CLAUDE.md), but no Flutter integration exists yet. This plan creates a Flutter test app that renders hardware-decoded H.264 video frames to a Flutter `Texture` widget using D3D11 GPU surface sharing — zero CPU copies.

## Architecture

```
Flutter Texture Widget (Dart)
    ↑ textureId
    |
MethodChannel "com.lumen/decoder"
    ↑ startDecode / stopDecode / getStats
    |
LumenDecoderPlugin (C++ Flutter plugin)
    |
    ├─ LumenTextureBridge (core class)
    │   ├─ D3D11DeviceContext      (Lumen — shared GPU device)
    │   ├─ MfVideoDecoder          (Lumen — HW H.264 decode)
    │   ├─ D3D11ColorConverter     (Lumen — NV12→BGRA on GPU)
    │   ├─ Shared BGRA texture     (D3D11_RESOURCE_MISC_SHARED)
    │   └─ Background decode thread
    |
    └─ flutter::GpuSurfaceTexture
        (kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle)
```

**Texture sharing**: Lumen creates a BGRA `ID3D11Texture2D` with `D3D11_RESOURCE_MISC_SHARED`. The DXGI shared handle is passed to Flutter's `GpuSurfaceTexture` callback. Flutter/ANGLE imports the handle via `OpenSharedResource` on its own D3D11 device. After each decoded+converted frame, we call `Flush()` on Lumen's device context to ensure GPU writes complete, then `MarkTextureFrameAvailable()`.

## File Structure

```
flutter_test_app/                              # NEW — Flutter project
  pubspec.yaml
  lib/
    main.dart                                  # Texture widget + stats overlay
    lumen_player.dart                          # MethodChannel wrapper
  windows/
    CMakeLists.txt                             # MODIFIED — add plugin sources + link lumen
    runner/
      main.cpp                                 # (generated, untouched)
      flutter_window.cpp                       # MODIFIED — register LumenDecoderPlugin
      flutter_window.h
    lumen/                                     # NEW — native plugin code
      lumen_decoder_plugin.h
      lumen_decoder_plugin.cpp                 # MethodChannel handler + texture registration
      lumen_texture_bridge.h                   # Core: decode pipeline + shared texture
      lumen_texture_bridge.cpp
      h264_file_reader.h                       # NAL parser extracted from decode_render_test
      h264_file_reader.cpp
```

Existing files modified:
```
platform/windows/common/d3d11_device_context.h     # Add CreateSharedBGRATexture()
platform/windows/common/d3d11_device_context.cpp   # Implement it
```

## Implementation Steps

### Step 1: Create Flutter project scaffold
```bash
cd D:\source\repos\lumen
flutter create --platforms=windows flutter_test_app
```

### Step 2: Add `CreateSharedBGRATexture` to D3D11DeviceContext
**Files**: `platform/windows/common/d3d11_device_context.h`, `.cpp`

Add method that creates BGRA texture with `D3D11_RESOURCE_MISC_SHARED` flag and returns both the texture and its DXGI shared handle:
```cpp
struct SharedTexture {
    ComPtr<ID3D11Texture2D> texture;
    HANDLE shared_handle;
};
Result<SharedTexture> CreateSharedBGRATexture(uint32_t width, uint32_t height);
```
Implementation: same as `CreateRGBATexture` but with `desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED`, then `QueryInterface<IDXGIResource>` → `GetSharedHandle(&handle)`.

### Step 3: Extract H264FileReader
**Files**: `flutter_test_app/windows/lumen/h264_file_reader.h`, `.cpp`

Extract from `tests/decode_render_test.cpp`:
- `NalUnit` struct, `FindNalUnits()`, `GroupAccessUnits()` (the fixed versions)
- Add `LoadH264File(path)` → returns bitstream + parsed access units

### Step 4: Implement LumenTextureBridge
**File**: `flutter_test_app/windows/lumen/lumen_texture_bridge.h`, `.cpp`

Core class that owns the pipeline and decode thread:

```cpp
class LumenTextureBridge {
public:
    LumenTextureBridge(flutter::TextureRegistrar* registrar);
    ~LumenTextureBridge();

    // Returns Flutter texture ID. Starts background decode thread.
    int64_t StartDecode(const std::string& h264_path,
                        uint32_t width, uint32_t height, uint32_t fps);
    void StopDecode();
    flutter::EncodableMap GetStats() const;

private:
    const FlutterDesktopGpuSurfaceDescriptor* ObtainDescriptor(
        size_t width, size_t height);
    void DecodeLoop(std::string path, uint32_t width,
                    uint32_t height, uint32_t fps);

    flutter::TextureRegistrar* texture_registrar_;
    std::unique_ptr<flutter::TextureVariant> gpu_texture_;
    int64_t texture_id_ = -1;

    // Lumen pipeline (created on decode thread for COM threading)
    std::unique_ptr<lumen::D3D11DeviceContext> device_ctx_;
    std::unique_ptr<lumen::MfVideoDecoder> decoder_;
    std::unique_ptr<lumen::D3D11ColorConverter> converter_;

    // Shared output texture
    lumen::ComPtr<ID3D11Texture2D> shared_bgra_;
    HANDLE shared_handle_ = nullptr;
    FlutterDesktopGpuSurfaceDescriptor surface_desc_ = {};

    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    // ... stats fields ...
};
```

**Decode loop** (background thread):
1. `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` + `MFStartup()`
2. Initialize `D3D11DeviceContext`, `MfVideoDecoder`, `D3D11ColorConverter`
3. Create shared BGRA texture via `CreateSharedBGRATexture()`
4. Store shared handle in `surface_desc_`
5. Load and parse H.264 file
6. For each access unit:
   - `decoder_->Decode(au_data, au_size, idx)` → NV12 texture
   - `converter_->Convert(decoded.native_texture, shared_bgra_.Get())`
   - `{ ScopedLock } device_ctx_->Context()->Flush()` (ensure GPU write completes)
   - `decoder_->ReleaseFrame(decoded)`
   - `texture_registrar_->MarkTextureFrameAvailable(texture_id_)`
   - Sleep to pace at target FPS
7. Cleanup: `decoder_->Flush()`, `MFShutdown()`, `CoUninitialize()`

**ObtainDescriptor callback** (called from Flutter render thread):
- Returns `&surface_desc_` (pre-filled with shared handle, dimensions, BGRA format)
- No D3D11 calls needed — just returns the cached descriptor

### Step 5: Implement LumenDecoderPlugin
**File**: `flutter_test_app/windows/lumen/lumen_decoder_plugin.h`, `.cpp`

Standard Flutter C++ plugin pattern:
```cpp
class LumenDecoderPlugin : public flutter::Plugin {
public:
    static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

private:
    LumenDecoderPlugin(flutter::TextureRegistrar* texture_registrar);
    void HandleMethodCall(const flutter::MethodCall<>& call,
                          std::unique_ptr<flutter::MethodResult<>>& result);

    std::unique_ptr<LumenTextureBridge> bridge_;
};
```

MethodChannel `"com.lumen/decoder"`:
- `startDecode({path, width, height, fps})` → returns `textureId` (int64)
- `stopDecode` → void
- `getStats` → `{framesDecoded, avgDecodeMs, avgConvertMs, fps}`

### Step 6: Wire plugin into Flutter runner
**File**: `flutter_test_app/windows/runner/flutter_window.cpp`

Add `#include "../lumen/lumen_decoder_plugin.h"` and register plugin in `FlutterWindow::OnCreate()`:
```cpp
LumenDecoderPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarManager::GetInstance()
        ->GetRegistrar<flutter::PluginRegistrarWindows>(
            flutter_controller_->engine()->GetRegistrarForPlugin("LumenDecoderPlugin")));
```

### Step 7: CMake integration
**File**: `flutter_test_app/windows/CMakeLists.txt`

Add after the existing runner target:
```cmake
# Lumen decode plugin
set(LUMEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Add lumen as subdirectory to get lumen_platform_windows target
add_subdirectory("${LUMEN_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/lumen")

# Plugin sources
target_sources(${BINARY_NAME} PRIVATE
    "lumen/lumen_decoder_plugin.cpp"
    "lumen/lumen_texture_bridge.cpp"
    "lumen/h264_file_reader.cpp"
)

target_include_directories(${BINARY_NAME} PRIVATE
    "${LUMEN_ROOT}/platform/windows"
    "${LUMEN_ROOT}/src/common/include"
    "${LUMEN_ROOT}/src/codec/include"
    "${LUMEN_ROOT}/src/renderer/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${BINARY_NAME} PRIVATE
    lumen_platform_windows
    mfplat mfuuid mf
)
```

### Step 8: Dart side
**File**: `flutter_test_app/lib/lumen_player.dart`
```dart
class LumenPlayer {
  static const _channel = MethodChannel('com.lumen/decoder');

  Future<int> startDecode(String path, {int width = 1920, int height = 1080, int fps = 60});
  Future<void> stopDecode();
  Future<Map<String, dynamic>> getStats();
}
```

**File**: `flutter_test_app/lib/main.dart`
- `Texture(textureId: _textureId)` widget to display video
- Start/stop button
- Stats overlay updated via periodic timer (polls `getStats()` every second)
- Hardcoded path to `output.h264` (placed alongside the executable)

### Step 9: Copy test asset and run
```bash
cp output.h264 flutter_test_app/build/windows/x64/runner/Release/
cd flutter_test_app
flutter run -d windows --release
```

## Key Design Decisions

1. **GPU surface type**: Use `kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle` (not `D3d11Texture2D`) because Lumen and Flutter use separate D3D11 devices. Shared handles work reliably across devices on the same GPU.

2. **COM threading**: The decode thread initializes its own MTA (`COINIT_MULTITHREADED`) for Media Foundation. Flutter's main thread uses STA. These don't conflict.

3. **Single-buffered**: One shared BGRA texture. After `Flush()` on Lumen's device, the write is guaranteed complete before Flutter reads. Acceptable for a test app. Production would double-buffer.

4. **Plugin in runner**: Rather than a separate plugin package, we add the C++ sources directly to the Flutter app's `windows/` build. Simpler for a test app.

## Critical Files Reference

| Existing File | What We Need From It |
|---|---|
| `platform/windows/common/d3d11_device_context.h/.cpp` | GPU device management, new `CreateSharedBGRATexture()` |
| `platform/windows/codec/mf_video_decoder.h/.cpp` | `MfVideoDecoder` class |
| `platform/windows/codec/d3d11_color_converter.h/.cpp` | `D3D11ColorConverter` class |
| `src/common/include/lumen/common/types.h` | `DecodedFrame`, `VideoDecoderConfig`, `PixelFormat` |
| `src/common/include/lumen/common/error.h` | `Result<T>`, `Error` |
| `tests/decode_render_test.cpp` | NAL parser to extract (`NalUnit`, `FindNalUnits`, `GroupAccessUnits`) |
| `platform/windows/CMakeLists.txt` | `lumen_platform_windows` static lib target |

## Verification
1. `flutter run -d windows` — app launches with video player widget
2. Decoded video frames render in the Texture widget at ~60 FPS
3. Stats overlay shows decode + convert latency per frame
4. No console errors beyond initial decoder warmup (~2-3 "needs more input")
5. Closing the app cleanly stops the decode thread
