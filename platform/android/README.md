# Lumen Android backend

Native Android backend behind the abstract pipeline interfaces. See
`docs/android-implementation-plan.md` for the full A0–A7 breakdown.

## Status

| Piece | State |
|---|---|
| Build plumbing + portable core (A0) | ✅ NDK-compiles (arm64-v8a) |
| quiche + BoringSSL transport (A1) | ✅ cross-compiled + links |
| Opus codec (A2) | ✅ via vcpkg `arm64-android` |
| AMediaCodec video enc/dec (A3) | ✅ compiles + links (`mediandk`) |
| JNI bridge `liblumen_android.so` (A6) | ✅ builds |
| Kotlin/Manifest app shell (A6) | ⚠️ skeleton — needs gradle/flutter + device |
| Capture / render / device e2e (A4/A5/A7) | ⛔ needs a device/emulator |

## Build the native libraries

```sh
# one-time: Opus for Android
ANDROID_NDK_HOME=<ndk> vcpkg install --triplet arm64-android --x-install-root=vcpkg_installed

cmake -S . -B build-android -G Ninja \
  -DCMAKE_MAKE_PROGRAM=<sdk>/cmake/3.22.1/bin/ninja.exe \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed/arm64-android \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
cmake --build build-android --target lumen_android      # -> liblumen_android.so
cmake --build build-android --target android_quiche_linkcheck   # full-chain link check
```

## JNI surface

`platform/android/jni/lumen_jni.cpp` ⇄ `LumenNative.kt`
(`com.viewsonic.lumen`). A6 exposes `quicheVersion()` and `selfTest()`; the
capture/encode/transport controls + MediaProjection token handoff grow here.

## App-shell wiring (device side — to do where flutter + a device exist)

The committed files (`AndroidManifest.xml`, `MainActivity.kt`, `LumenNative.kt`)
are the stable skeleton. The version-fragile Gradle/Flutter pieces are
intentionally **not** committed; generate/author them on a machine with the
Flutter toolchain:

1. `flutter create --platforms=android .` from `flutter_test_app/` to generate
   `android/` Gradle scaffolding (then keep the committed manifest/kotlin).
2. Make `liblumen_android.so` available to the app — either:
   - drop the built `.so` into `android/app/src/main/jniLibs/arm64-v8a/`, or
   - add an `externalNativeBuild { cmake { path "../../../CMakeLists.txt" } }`
     block driving this CMake (passes `ANDROID_ABI`/`-DCMAKE_PREFIX_PATH`).
3. MediaProjection flow (A4): from `MainActivity`, request the projection
   intent via `MediaProjectionManager`, start the `mediaProjection` foreground
   service, and pass the resulting `Surface`/token to the native capture
   backend over a MethodChannel.

## Verification

- A0–A3 + the JNI `.so`: **compile + link** verified via `build-android` on a
  Windows host (no device).
- Functional capture→encode→transport→decode→render: **needs a device/emulator**
  (A7 instrumented test).
