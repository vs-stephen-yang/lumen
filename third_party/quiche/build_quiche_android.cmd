@echo off
rem Cross-compile quiche (+ bundled BoringSSL) for Android from a Windows host.
rem Invoked by third_party/quiche/CMakeLists.txt under if(ANDROID). Encodes the
rem env recipe verified in docs/android-implementation-plan.md (A1).
rem
rem Args: NDK rust_triple clang_prefix api profile manifest cargo ninja_dir triple_uc
setlocal
set "NDK=%~1"
set "TR=%~2"
set "CLP=%~3"
set "API=%~4"
set "PROFILE=%~5"
set "MANIFEST=%~6"
set "CARGO=%~7"
set "NINJADIR=%~8"
set "TRUC=%~9"

rem Lower-underscore form for the cc-crate CC_/CXX_/AR_<triple> keys.
set "TRUS=%TR:-=_%"
set "TCBIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin"

set "ANDROID_NDK_HOME=%NDK%"
set "ANDROID_NDK_ROOT=%NDK%"
rem Force Ninja for the BoringSSL CMake build (else CMake picks VS → MSBuild).
set "CMAKE_GENERATOR=Ninja"
set "PATH=%NINJADIR%;%TCBIN%;%PATH%"
rem cargo expects the upper-case underscored triple in this key.
set "CARGO_TARGET_%TRUC%_LINKER=%CLP%%API%-clang.cmd"
set "CARGO_TARGET_%TRUC%_AR=llvm-ar.exe"
set "CC_%TRUS%=%CLP%%API%-clang.cmd"
set "CXX_%TRUS%=%CLP%%API%-clang++.cmd"
set "AR_%TRUS%=llvm-ar.exe"

"%CARGO%" build --target %TR% --profile %PROFILE% --features ffi --manifest-path "%MANIFEST%"
exit /b %ERRORLEVEL%
