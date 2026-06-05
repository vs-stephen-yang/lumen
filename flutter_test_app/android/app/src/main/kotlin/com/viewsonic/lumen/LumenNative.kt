package com.viewsonic.lumen

/**
 * Kotlin façade over the native Lumen pipeline (liblumen_android.so).
 *
 * The `external` signatures must match the JNI exports in
 * platform/android/jni/lumen_jni.cpp. A6 skeleton — `quicheVersion`/`selfTest`
 * confirm the native library loaded and links; capture/encode/transport
 * controls and the MediaProjection token handoff are added in A4/A7.
 */
object LumenNative {
    init {
        System.loadLibrary("lumen_android")
    }

    /** quiche version string — proves the native lib is reachable. */
    external fun quicheVersion(): String

    /** Links + smoke-checks the core (POSIX socket, jitter, opus, codec). */
    external fun selfTest(): Boolean
}
