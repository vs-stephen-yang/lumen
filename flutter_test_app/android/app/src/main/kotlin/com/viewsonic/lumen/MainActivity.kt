package com.viewsonic.lumen

import io.flutter.embedding.android.FlutterActivity

/**
 * Flutter host activity (A6 skeleton). The MediaProjection permission flow
 * (startActivityForResult on the MediaProjectionManager intent) and the
 * foreground-service token handoff to the native capture backend (A4) are
 * wired here via a MethodChannel once the device path is brought up.
 */
class MainActivity : FlutterActivity()
