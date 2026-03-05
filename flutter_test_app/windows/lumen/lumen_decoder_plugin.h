#pragma once

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

class LumenTextureBridge;

/// Flutter plugin that exposes Lumen's zero-copy decode pipeline via
/// a MethodChannel. Registers a GPU surface texture backed by a
/// DXGI shared handle for zero-copy rendering in the Flutter Texture widget.
class LumenDecoderPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar);

  explicit LumenDecoderPlugin(flutter::TextureRegistrar* texture_registrar);
  ~LumenDecoderPlugin() override;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  flutter::TextureRegistrar* texture_registrar_;
  std::unique_ptr<LumenTextureBridge> bridge_;
};
