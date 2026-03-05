import 'package:flutter/services.dart';

/// MethodChannel wrapper for the native Lumen decode plugin.
class LumenPlayer {
  static const _channel = MethodChannel('com.lumen/decoder');

  /// Start decoding an H.264 file. Returns the Flutter texture ID.
  Future<int> startDecode(
    String path, {
    int width = 1920,
    int height = 1080,
    int fps = 60,
  }) async {
    final textureId = await _channel.invokeMethod<int>('startDecode', {
      'path': path,
      'width': width,
      'height': height,
      'fps': fps,
    });
    return textureId ?? -1;
  }

  /// Stop the current decode session.
  Future<void> stopDecode() async {
    await _channel.invokeMethod<void>('stopDecode');
  }

  /// Get current decode statistics.
  Future<Map<String, dynamic>> getStats() async {
    final result = await _channel.invokeMethod<Map>('getStats');
    if (result == null) return {};
    return result.cast<String, dynamic>();
  }
}
