# Android Backend — Subtask Breakdown (Phase 2)

Android is the first native platform after Windows. The portable core (abstract
interfaces, transport framing, **jitter buffer**, and the **POSIX UDP socket**)
is already in place; this phase adds the Android-specific backends behind those
same interfaces.

## Environment (probed 2026-06-04 on this Windows dev box)

| Tool | State | Note |
|---|---|---|
| Android NDK | ✅ `…/Android/Sdk/ndk/28.2.13676358` (+23–27) | use a recent one |
| Android SDK / adb | ✅ present | for device/emulator |
| JDK | ✅ 17 (MS hotspot) | Gradle/Flutter |
| cargo / rustc | ✅ at `~/.cargo/bin` | not on bash PATH |
| Rust android targets | ❌ none | need `rustup target add …-linux-android` |
| gradle | ❌ not on PATH | Flutter uses `./gradlew` |
| opus | ⚠️ vcpkg/Windows only | needs an Android build |

**Verifiable here** = compiles via the NDK on this box (no device).
**Needs device** = functional behaviour requires an emulator/physical device.

## Subtasks (ordered; dependencies noted)

| # | Subtask | Prereqs / installs | Verifiable here | Needs device |
|---|---|---|---|---|
| **A0** | **Build plumbing + portable-core NDK compile.** Make root `CMakeLists` Android-aware (`if(ANDROID)` adds `platform/posix` + `platform/android`; skip Windows-only platform & tests; gate `third_party/quiche` off until A1). Add `platform/android/CMakeLists.txt` → `lumen_platform_android`. NDK-configure a `build-android/` for `arm64-v8a` and compile common + transport framing + jitter_buffer + **posix UDP socket**. | NDK path | ✅ (validates the Phase 1b POSIX socket for real) | — |
| **A1** | **quiche for Android.** Teach the cargo bridge (`third_party/quiche/CMakeLists.txt`) to cross-compile with `--target <abi>-linux-android`, NDK clang as linker; emit a per-ABI static lib. Link a tiny `quiche_smoke` under NDK. | `rustup target add aarch64-linux-android` (+ armv7/x86_64); NDK linker env | ✅ (builds + links) | — |
| **A2** | **opus for Android.** Build/obtain libopus for the Android ABIs (vcpkg `arm64-android` triplet, or NDK build) so `lumen_codec` compiles for Android. | opus android build | ✅ | — |
| **A3** | **Video encode/decode (AMediaCodec).** `AMediaCodecVideoEncoder`/`Decoder` (NDK `media/NdkMediaCodec`), H.264/HEVC, zero-copy via `ANativeWindow`/`AHardwareBuffer`. Implements `VideoEncoder`/`VideoDecoder`; unit-test error paths with Phase 0 mocks. | A0 | ✅ compile; contract test | functional decode/encode |
| **A4** | **Capture.** Screen: `MediaProjection` → `ImageReader`/`Surface` (needs Kotlin/Java + JNI + a runtime permission UI). System audio: `AudioPlaybackCapture` (API 29+). Implements `ScreenCapture`/`AudioCapture`. | A0; app shell (A6) for the permission grant | ⚠️ native parts compile | yes (permission + capture) |
| **A5** | **Render.** Video: OpenGL ES 2/3 (or Vulkan) sampling `AHardwareBuffer` to a `SurfaceView`; implements `VideoRenderer`. Audio: AAudio playback; implements `AudioRenderer`. | A0 | ⚠️ compile | yes (display/audio out) |
| **A6** | **App shell + JNI + Flutter.** Add the Android Flutter runner (today only `flutter_test_app/windows`), JNI bridge exposing the pipeline to Dart/FFI, and the MediaProjection permission flow. Wires capture→encode→transport→decode→render. | A0–A5; gradle wrapper | partial | yes |
| **A7** | **On-device e2e + instrumented test.** Bring up the full pipeline on an emulator/device; add an instrumented test mirroring `web_e2e_test`. | A6; device/emulator | — | yes |

### Dependency graph
```
A0 ──┬─> A1 (quiche) ─┐
     ├─> A2 (opus) ───┤
     └─> A3 (codec) ──┴─> A4 (capture) ─┐
                          A5 (render) ──┴─> A6 (app/JNI/Flutter) ─> A7 (device e2e)
```
Transport on Android needs A1; audio needs A2.

## Recommended execution order
1. **A0** — pure build plumbing, validates the POSIX socket under the real NDK, no installs. Lowest risk, do first.
2. **A1 + A2** — unlock transport and audio codec (require the rust-android target install and an opus-android build respectively).
3. **A3** — MediaCodec video, compile-verifiable + mock contract tests.
4. **A4 / A5** — capture + render (native compiles here; behaviour on device).
5. **A6 / A7** — app shell, JNI, Flutter runner, on-device e2e.

## Verification strategy
- A0–A3 (and the native halves of A4/A5) are NDK-**compile**-verified on this box
  via a dedicated `build-android/` configure (separate from the MSVC `build/`).
- Functional behaviour (capture, render, full pipeline) is validated on an
  emulator/device from A4 onward.
- The Windows MSVC build + its test suite (and the Stop hook) stay green
  throughout — Android wiring lives entirely under `if(ANDROID)`.

## Notes / decisions needed
- **Rust android targets** (A1): requires `rustup target add` — an environment
  change to approve.
- **opus-android** (A2): pick vcpkg android triplet vs NDK build.
- **App shell** (A6): Flutter Android module vs a standalone Gradle+NDK app.
