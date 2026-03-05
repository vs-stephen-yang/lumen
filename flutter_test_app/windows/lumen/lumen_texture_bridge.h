#pragma once

#include <flutter/encodable_value.h>
#include <flutter/texture_registrar.h>

#include "lumen/common/com_ptr.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct ID3D11Texture2D;

namespace lumen {
class D3D11DeviceContext;
class MfVideoDecoder;
class D3D11ColorConverter;
}  // namespace lumen

/// Bridges Lumen's zero-copy decode pipeline to a Flutter GPU texture.
///
/// Creates a shared BGRA texture (D3D11_RESOURCE_MISC_SHARED), runs the
/// decode → color-convert loop on a background thread, and exposes the
/// DXGI shared handle to Flutter via GpuSurfaceTexture.
class LumenTextureBridge {
 public:
  explicit LumenTextureBridge(flutter::TextureRegistrar* registrar);
  ~LumenTextureBridge();

  LumenTextureBridge(const LumenTextureBridge&) = delete;
  LumenTextureBridge& operator=(const LumenTextureBridge&) = delete;

  /// Start decoding an H.264 file and rendering to the Flutter texture.
  /// Returns the Flutter texture ID, or -1 on failure.
  int64_t StartDecode(const std::string& h264_path, uint32_t width,
                      uint32_t height, uint32_t fps);

  /// Stop the decode loop and release resources.
  void StopDecode();

  /// Get current stats as a Flutter-encodable map.
  flutter::EncodableMap GetStats() const;

 private:
  const FlutterDesktopGpuSurfaceDescriptor* ObtainDescriptor(size_t width,
                                                              size_t height);
  void DecodeLoop(std::string path, uint32_t width, uint32_t height,
                  uint32_t fps);

  flutter::TextureRegistrar* texture_registrar_;
  std::unique_ptr<flutter::TextureVariant> gpu_texture_;
  int64_t texture_id_ = -1;

  // Lumen pipeline — created on decode thread for COM threading.
  std::unique_ptr<lumen::D3D11DeviceContext> device_ctx_;
  std::unique_ptr<lumen::MfVideoDecoder> decoder_;
  std::unique_ptr<lumen::D3D11ColorConverter> converter_;

  // Shared output texture.
  lumen::ComPtr<ID3D11Texture2D> shared_bgra_;
  HANDLE shared_handle_ = nullptr;
  FlutterDesktopGpuSurfaceDescriptor surface_desc_ = {};

  std::thread decode_thread_;
  std::atomic<bool> running_{false};

  // Stats — updated from decode thread, read from main thread.
  mutable std::mutex stats_mutex_;
  uint32_t frames_decoded_ = 0;
  double total_decode_ms_ = 0.0;
  double total_convert_ms_ = 0.0;
  std::chrono::steady_clock::time_point decode_start_time_;
};
