#include <jni.h>
#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include "engine_host.h"

using mge::engineHost;

namespace {
jobject g_assetManagerRef = nullptr;   // keep Java AssetManager alive for the native pointer
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_glammer_mge_NativeEngine_create(JNIEnv* env, jobject, jobject assets, jstring dataPath) {
    if (g_assetManagerRef) env->DeleteGlobalRef(g_assetManagerRef);
    g_assetManagerRef = env->NewGlobalRef(assets);
    const char* path = env->GetStringUTFChars(dataPath, nullptr);
    bool ok = engineHost().create(AAssetManager_fromJava(env, g_assetManagerRef), path);
    env->ReleaseStringUTFChars(dataPath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_surfaceCreated(JNIEnv* env, jobject, jobject surface) {
    engineHost().setWindow(ANativeWindow_fromSurface(env, surface));
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_surfaceChanged(JNIEnv*, jobject, jint w, jint h) {
    engineHost().resize(w, h);
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_setInsets(JNIEnv*, jobject, jint l, jint t, jint r, jint b) {
    engineHost().setInsets(l, t, r, b);
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_surfaceDestroyed(JNIEnv*, jobject) {
    engineHost().setWindow(nullptr);
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_pause(JNIEnv*, jobject) { engineHost().setPaused(true); }

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_resume(JNIEnv*, jobject) { engineHost().setPaused(false); }

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_destroy(JNIEnv* env, jobject) {
    engineHost().destroy();
    if (g_assetManagerRef) { env->DeleteGlobalRef(g_assetManagerRef); g_assetManagerRef = nullptr; }
}

}
