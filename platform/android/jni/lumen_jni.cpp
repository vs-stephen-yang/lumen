// JNI bridge between the Android app (Kotlin/Flutter) and the native Lumen
// pipeline. A6 skeleton: exposes a version string and a self-test so the app
// can confirm the native library loaded and links. The real surface (start/
// stop capture→encode→transport, MediaProjection token handoff, decoder output
// Surface) grows here as A4/A5/A7 land.
//
// Java class: com.viewsonic.lumen.LumenNative

#include <jni.h>

#include <quiche.h>

namespace lumen {
bool AndroidCoreLinkCheck();  // platform/android/android_backend.cpp
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL
Java_com_viewsonic_lumen_LumenNative_quicheVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(quiche_version());
}

JNIEXPORT jboolean JNICALL
Java_com_viewsonic_lumen_LumenNative_selfTest(JNIEnv* /*env*/, jclass) {
    return lumen::AndroidCoreLinkCheck() ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
