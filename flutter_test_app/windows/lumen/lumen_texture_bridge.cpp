#include "lumen_texture_bridge.h"
#include "h264_file_reader.h"

#include "common/d3d11_device_context.h"
#include "codec/d3d11_color_converter.h"
#include "codec/mf_video_decoder.h"

#include <mfapi.h>

#include <chrono>

using Clock = std::chrono::high_resolution_clock;

LumenTextureBridge::LumenTextureBridge(
    flutter::TextureRegistrar* registrar)
    : texture_registrar_(registrar) {}

LumenTextureBridge::~LumenTextureBridge() { StopDecode(); }

int64_t LumenTextureBridge::StartDecode(const std::string& h264_path,
                                         uint32_t width, uint32_t height,
                                         uint32_t fps) {
  if (running_.load()) {
    StopDecode();
  }

  // Register the GPU surface texture with Flutter before starting decode.
  // The actual shared handle will be set once the decode thread creates it.
  gpu_texture_ = std::make_unique<flutter::TextureVariant>(
      flutter::GpuSurfaceTexture(
          kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
          [this](size_t w, size_t h) { return ObtainDescriptor(w, h); }));

  texture_id_ = texture_registrar_->RegisterTexture(gpu_texture_.get());
  if (texture_id_ < 0) {
    gpu_texture_.reset();
    return -1;
  }

  running_.store(true);
  decode_thread_ =
      std::thread(&LumenTextureBridge::DecodeLoop, this, h264_path, width,
                  height, fps);

  return texture_id_;
}

void LumenTextureBridge::StopDecode() {
  running_.store(false);
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }
  if (texture_id_ >= 0) {
    texture_registrar_->UnregisterTexture(texture_id_);
    texture_id_ = -1;
  }
  gpu_texture_.reset();
}

flutter::EncodableMap LumenTextureBridge::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  flutter::EncodableMap stats;
  stats[flutter::EncodableValue("framesDecoded")] =
      flutter::EncodableValue(static_cast<int32_t>(frames_decoded_));

  double avg_decode =
      frames_decoded_ > 0 ? total_decode_ms_ / frames_decoded_ : 0.0;
  double avg_convert =
      frames_decoded_ > 0 ? total_convert_ms_ / frames_decoded_ : 0.0;

  stats[flutter::EncodableValue("avgDecodeMs")] =
      flutter::EncodableValue(avg_decode);
  stats[flutter::EncodableValue("avgConvertMs")] =
      flutter::EncodableValue(avg_convert);

  double elapsed_s = 0.0;
  if (frames_decoded_ > 0) {
    auto now = std::chrono::steady_clock::now();
    elapsed_s = std::chrono::duration<double>(now - decode_start_time_).count();
  }
  double fps = elapsed_s > 0.0 ? frames_decoded_ / elapsed_s : 0.0;
  stats[flutter::EncodableValue("fps")] = flutter::EncodableValue(fps);

  return stats;
}

const FlutterDesktopGpuSurfaceDescriptor* LumenTextureBridge::ObtainDescriptor(
    size_t /*width*/, size_t /*height*/) {
  // Return the pre-filled descriptor. The shared handle and dimensions
  // are set once by the decode thread and remain constant.
  if (shared_handle_ == nullptr) {
    return nullptr;
  }
  return &surface_desc_;
}

void LumenTextureBridge::DecodeLoop(std::string path, uint32_t width,
                                     uint32_t height, uint32_t fps) {
  // Initialize COM for Media Foundation on this thread.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);

  // Create Lumen pipeline components.
  device_ctx_ = std::make_unique<lumen::D3D11DeviceContext>();
  auto result = device_ctx_->Initialize(0);
  if (!result) {
    OutputDebugStringA(
        ("LumenTextureBridge: Device init failed: " +
         result.error().message + "\n")
            .c_str());
    MFShutdown();
    CoUninitialize();
    return;
  }

  // Create shared BGRA texture for Flutter.
  auto shared_result = device_ctx_->CreateSharedBGRATexture(width, height);
  if (!shared_result) {
    OutputDebugStringA(
        ("LumenTextureBridge: Shared texture failed: " +
         shared_result.error().message + "\n")
            .c_str());
    MFShutdown();
    CoUninitialize();
    return;
  }
  shared_bgra_ = std::move(shared_result.value().texture);
  shared_handle_ = shared_result.value().shared_handle;

  // Fill in the surface descriptor.
  surface_desc_.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
  surface_desc_.handle = shared_handle_;
  surface_desc_.width = width;
  surface_desc_.height = height;
  surface_desc_.visible_width = width;
  surface_desc_.visible_height = height;
  surface_desc_.format = kFlutterDesktopPixelFormatBGRA8888;
  surface_desc_.release_callback = nullptr;
  surface_desc_.release_context = nullptr;

  // Initialize decoder.
  decoder_ = std::make_unique<lumen::MfVideoDecoder>(*device_ctx_);
  lumen::VideoDecoderConfig dec_config;
  dec_config.codec = lumen::VideoCodec::kH264;
  dec_config.width = width;
  dec_config.height = height;
  dec_config.fps = fps;
  dec_config.low_latency = true;

  result = decoder_->Initialize(dec_config, nullptr);
  if (!result) {
    OutputDebugStringA(
        ("LumenTextureBridge: Decoder init failed: " +
         result.error().message + "\n")
            .c_str());
    MFShutdown();
    CoUninitialize();
    return;
  }

  // Initialize color converter (NV12 → BGRA).
  converter_ = std::make_unique<lumen::D3D11ColorConverter>(*device_ctx_);
  lumen::VideoFrameDesc nv12_desc;
  nv12_desc.width = width;
  nv12_desc.height = height;
  nv12_desc.format = lumen::PixelFormat::kNV12;

  result = converter_->Initialize(nv12_desc, lumen::PixelFormat::kBGRA, nullptr);
  if (!result) {
    OutputDebugStringA(
        ("LumenTextureBridge: Converter init failed: " +
         result.error().message + "\n")
            .c_str());
    MFShutdown();
    CoUninitialize();
    return;
  }

  // Load and parse H.264 file.
  auto bitstream = lumen::LoadH264File(path);
  if (bitstream.data.empty() || bitstream.access_units.empty()) {
    OutputDebugStringA("LumenTextureBridge: Failed to load H.264 file\n");
    MFShutdown();
    CoUninitialize();
    return;
  }

  OutputDebugStringA(
      ("LumenTextureBridge: Loaded " +
       std::to_string(bitstream.access_units.size()) +
       " access units, starting decode\n")
          .c_str());

  // Record start time for FPS calculation.
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    decode_start_time_ = std::chrono::steady_clock::now();
  }

  auto frame_duration = std::chrono::microseconds(1'000'000 / fps);

  // Decode loop.
  for (size_t au_idx = 0;
       au_idx < bitstream.access_units.size() && running_.load(); ++au_idx) {
    auto frame_start = Clock::now();

    auto [nal_start, nal_end] = bitstream.access_units[au_idx];
    size_t au_offset = bitstream.nals[nal_start].offset;
    size_t au_end_offset = bitstream.nals[nal_end - 1].offset +
                           bitstream.nals[nal_end - 1].length;
    size_t au_size = au_end_offset - au_offset;

    // Decode.
    auto decode_start = Clock::now();
    auto decode_result = decoder_->Decode(
        bitstream.data.data() + au_offset, au_size, au_idx);
    auto after_decode = Clock::now();

    if (!decode_result) {
      if (decode_result.error().code == lumen::ErrorCode::kTimeout) {
        // Decoder needs more input — normal for first few frames.
        continue;
      }
      // Skip decode errors.
      continue;
    }

    auto& decoded_frame = decode_result.value();

    // Color convert NV12 → BGRA into the shared texture.
    auto convert_start = Clock::now();
    result = converter_->Convert(decoded_frame.native_texture,
                                  shared_bgra_.Get());
    auto after_convert = Clock::now();

    // Flush to ensure GPU write completes before Flutter reads.
    {
      lumen::D3D11DeviceContext::ScopedLock lock(*device_ctx_);
      device_ctx_->Context()->Flush();
    }

    decoder_->ReleaseFrame(decoded_frame);

    if (!result) {
      continue;
    }

    // Notify Flutter that a new frame is available.
    texture_registrar_->MarkTextureFrameAvailable(texture_id_);

    // Update stats.
    {
      std::lock_guard<std::mutex> slock(stats_mutex_);
      frames_decoded_++;
      total_decode_ms_ += std::chrono::duration<double, std::milli>(
                              after_decode - decode_start)
                              .count();
      total_convert_ms_ += std::chrono::duration<double, std::milli>(
                               after_convert - convert_start)
                               .count();
    }

    // Pace at target FPS.
    auto elapsed = Clock::now() - frame_start;
    if (elapsed < frame_duration) {
      std::this_thread::sleep_for(frame_duration - elapsed);
    }
  }

  // Flush remaining decoder output.
  decoder_->Flush();

  // Cleanup pipeline (must be done before COM uninit).
  converter_.reset();
  decoder_.reset();
  shared_bgra_.Reset();
  device_ctx_.reset();

  MFShutdown();
  CoUninitialize();
}
