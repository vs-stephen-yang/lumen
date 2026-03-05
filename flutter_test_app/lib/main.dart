import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'lumen_player.dart';

void main() {
  runApp(const LumenTestApp());
}

class LumenTestApp extends StatelessWidget {
  const LumenTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Lumen Decode Test',
      theme: ThemeData.dark(useMaterial3: true),
      home: const PlayerPage(),
    );
  }
}

class PlayerPage extends StatefulWidget {
  const PlayerPage({super.key});

  @override
  State<PlayerPage> createState() => _PlayerPageState();
}

class _PlayerPageState extends State<PlayerPage> {
  final _player = LumenPlayer();
  int _textureId = -1;
  bool _playing = false;
  Map<String, dynamic> _stats = {};
  Timer? _statsTimer;
  String _status = 'Stopped';

  @override
  void dispose() {
    _stopDecode();
    super.dispose();
  }

  Future<void> _startDecode() async {
    // Look for output.h264 next to the executable.
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    final h264Path = '$exeDir/output.h264';

    if (!File(h264Path).existsSync()) {
      setState(() {
        _status = 'Error: output.h264 not found in $exeDir';
      });
      return;
    }

    setState(() => _status = 'Starting...');

    try {
      final textureId = await _player.startDecode(h264Path);
      if (textureId >= 0) {
        setState(() {
          _textureId = textureId;
          _playing = true;
          _status = 'Playing';
        });
        // Poll stats every second.
        _statsTimer = Timer.periodic(
          const Duration(seconds: 1),
          (_) => _updateStats(),
        );
      } else {
        setState(() => _status = 'Error: Failed to start decode');
      }
    } catch (e) {
      setState(() => _status = 'Error: $e');
    }
  }

  Future<void> _stopDecode() async {
    _statsTimer?.cancel();
    _statsTimer = null;
    await _player.stopDecode();
    if (mounted) {
      setState(() {
        _textureId = -1;
        _playing = false;
        _status = 'Stopped';
        _stats = {};
      });
    }
  }

  Future<void> _updateStats() async {
    if (!_playing) return;
    final stats = await _player.getStats();
    if (mounted) {
      setState(() => _stats = stats);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Lumen Zero-Copy Decode Test'),
        actions: [
          IconButton(
            icon: Icon(_playing ? Icons.stop : Icons.play_arrow),
            onPressed: _playing ? _stopDecode : _startDecode,
            tooltip: _playing ? 'Stop' : 'Play',
          ),
        ],
      ),
      body: Stack(
        children: [
          // Video display.
          Center(
            child: _textureId >= 0
                ? AspectRatio(
                    aspectRatio: 16 / 9,
                    child: Texture(textureId: _textureId),
                  )
                : Text(
                    _status,
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
          ),
          // Stats overlay.
          if (_stats.isNotEmpty)
            Positioned(
              top: 8,
              left: 8,
              child: Container(
                padding: const EdgeInsets.all(8),
                decoration: BoxDecoration(
                  color: Colors.black54,
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    _statLine('Frames', '${_stats['framesDecoded'] ?? 0}'),
                    _statLine('Decode',
                        '${(_stats['avgDecodeMs'] ?? 0.0).toStringAsFixed(2)} ms'),
                    _statLine('Convert',
                        '${(_stats['avgConvertMs'] ?? 0.0).toStringAsFixed(2)} ms'),
                    _statLine(
                        'FPS', '${(_stats['fps'] ?? 0.0).toStringAsFixed(1)}'),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _statLine(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 1),
      child: Text(
        '$label: $value',
        style: const TextStyle(
          fontFamily: 'monospace',
          fontSize: 13,
          color: Colors.white,
        ),
      ),
    );
  }
}
