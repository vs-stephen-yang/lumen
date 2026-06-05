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

## A1 — verified cross-compile recipe (2026-06-05)

quiche (incl. bundled BoringSSL) **does** cross-compile for Android from this
Windows host. Proven manually for `aarch64-linux-android` → `libquiche.a`.
Required:
- `rustup target add aarch64-linux-android` (done).
- A `build.rs` fix (committed): `get_boringssl_platform_output_path()` keyed off
  `cfg!(target_env="msvc")`, which in a build script reflects the **host**, so
  it wrongly appended a `Release/` subdir for the single-config Ninja Android
  BoringSSL build. Now keys off `CARGO_CFG_TARGET_ENV` (the target). Windows
  build unchanged (target_env stays `msvc`).
- Build env (to encode into the CMake bridge in the remaining A1 work):
  ```
  ANDROID_NDK_HOME = <sdk>/ndk/28.2.13676358
  CMAKE_GENERATOR  = Ninja                      # else CMake picks VS → MSBuild fails
  PATH            += <sdk>/cmake/3.22.1/bin       # a real ninja.exe (NOT depot_tools)
                   + <ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin
  CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER = aarch64-linux-android29-clang.cmd
  CARGO_TARGET_AARCH64_LINUX_ANDROID_AR     = llvm-ar.exe
  CC/CXX/AR_aarch64_linux_android           = …29-clang(.cmd) / clang++ / llvm-ar
  cargo build --target aarch64-linux-android --features ffi \
      --manifest-path quiche/Cargo.toml --release
  ```
- **Remaining A1:** teach `third_party/quiche/CMakeLists.txt` to do the above
  under `if(ANDROID)` (ABI→triple map, env via `${CMAKE_COMMAND} -E env`, output
  at `target/<triple>/<profile>/libquiche.a`), re-enable `third_party` for
  Android in root CMake, link a `quiche_version()` check from
  `platform/android`.

## A2 — opus for Android (done)

`opus:arm64-android` built via vcpkg (only manifest dep), then `lumen_codec`
compiles + links for Android.
```
ANDROID_NDK_HOME=<ndk> vcpkg install --triplet arm64-android \
    --x-install-root=vcpkg_installed          # -> vcpkg_installed/arm64-android/lib/libopus.a
```
The NDK toolchain restricts `find_package` to the sysroot, so the Android
configure must point at the vcpkg prefix AND allow package search outside the
root.

## Full Android configure (A0–A2, arm64-v8a)
```
cmake -S . -B build-android -G Ninja \
  -DCMAKE_MAKE_PROGRAM=<sdk>/cmake/3.22.1/bin/ninja.exe \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_PREFIX_PATH=<repo>/vcpkg_installed/arm64-android \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
cmake --build build-android --target android_quiche_linkcheck
```
Builds + links core + POSIX socket + quiche + BoringSSL + libopus for Android.
(`build-android/` and `vcpkg_installed/` are gitignored; regenerate per the
above.)

## Notes / decisions needed
- **Rust android targets** (A1): requires `rustup target add` — an environment
  change to approve.
- **opus-android** (A2): pick vcpkg android triplet vs NDK build.
- **App shell** (A6): Flutter Android module vs a standalone Gradle+NDK app.
