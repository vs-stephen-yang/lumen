#include "lumen_decoder_plugin.h"
#include "lumen_texture_bridge.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

void LumenDecoderPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "com.lumen/decoder",
      &flutter::StandardMethodCodec::GetInstance());

  auto* texture_registrar = registrar->texture_registrar();

  auto plugin = std::make_unique<LumenDecoderPlugin>(texture_registrar);

  channel->SetMethodCallHandler(
      [plugin_ptr = plugin.get()](
          const flutter::MethodCall<flutter::EncodableValue>& call,
          std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
              result) { plugin_ptr->HandleMethodCall(call, std::move(result)); });

  registrar->AddPlugin(std::move(plugin));
}

LumenDecoderPlugin::LumenDecoderPlugin(
    flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {}

LumenDecoderPlugin::~LumenDecoderPlugin() = default;

void LumenDecoderPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto& method = call.method_name();

  if (method == "startDecode") {
    const auto* args =
        std::get_if<flutter::EncodableMap>(call.arguments());
    if (!args) {
      result->Error("INVALID_ARGS", "Expected a map argument");
      return;
    }

    auto path_it = args->find(flutter::EncodableValue("path"));
    auto width_it = args->find(flutter::EncodableValue("width"));
    auto height_it = args->find(flutter::EncodableValue("height"));
    auto fps_it = args->find(flutter::EncodableValue("fps"));

    if (path_it == args->end()) {
      result->Error("INVALID_ARGS", "Missing 'path' argument");
      return;
    }

    std::string path = std::get<std::string>(path_it->second);
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    if (width_it != args->end()) {
      width = static_cast<uint32_t>(std::get<int32_t>(width_it->second));
    }
    if (height_it != args->end()) {
      height = static_cast<uint32_t>(std::get<int32_t>(height_it->second));
    }
    if (fps_it != args->end()) {
      fps = static_cast<uint32_t>(std::get<int32_t>(fps_it->second));
    }

    // Stop any existing decode session.
    if (bridge_) {
      bridge_->StopDecode();
      bridge_.reset();
    }

    bridge_ = std::make_unique<LumenTextureBridge>(texture_registrar_);
    int64_t texture_id = bridge_->StartDecode(path, width, height, fps);

    if (texture_id < 0) {
      result->Error("DECODE_ERROR", "Failed to start decode pipeline");
      bridge_.reset();
      return;
    }

    result->Success(flutter::EncodableValue(texture_id));

  } else if (method == "stopDecode") {
    if (bridge_) {
      bridge_->StopDecode();
      bridge_.reset();
    }
    result->Success();

  } else if (method == "getStats") {
    if (bridge_) {
      result->Success(flutter::EncodableValue(bridge_->GetStats()));
    } else {
      result->Success(flutter::EncodableValue(flutter::EncodableMap()));
    }

  } else {
    result->NotImplemented();
  }
}
