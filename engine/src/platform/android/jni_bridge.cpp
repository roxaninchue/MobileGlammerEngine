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

// action: 0 down, 1 move, 2 up, 3 cancel. For down/up only the pointer at actionIndex changed;
// for move every pointer reports its position. xy holds count (x, y) pairs in surface pixels.
JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_touch(JNIEnv* env, jobject, jint action, jint actionIndex, jint count,
                                        jintArray ids, jfloatArray xy, jlong timeMs) {
    using mge::input::TouchAction;
    using mge::input::TouchEvent;
    constexpr int kMax = mge::input::kMaxPointers;
    if (count < 0) return;
    if (count > kMax) count = kMax;
    jint id[kMax];
    jfloat pos[2 * kMax];
    env->GetIntArrayRegion(ids, 0, count, id);
    env->GetFloatArrayRegion(xy, 0, 2 * count, pos);
    const int64_t t = int64_t(timeMs) * 1'000'000;
    switch (action) {
    case 0:
    case 2:
        if (actionIndex >= 0 && actionIndex < count)
            engineHost().touch(TouchEvent{action == 0 ? TouchAction::Down : TouchAction::Up, id[actionIndex],
                                          {pos[2 * actionIndex], pos[2 * actionIndex + 1]}, t});
        break;
    case 1:
        for (int i = 0; i < count; ++i)
            engineHost().touch(TouchEvent{TouchAction::Move, id[i], {pos[2 * i], pos[2 * i + 1]}, t});
        break;
    case 3:
        engineHost().touch(TouchEvent{TouchAction::Cancel, -1, {}, t});
        break;
    default: break;
    }
}

JNIEXPORT void JNICALL
Java_com_glammer_mge_NativeEngine_setDensity(JNIEnv*, jobject, jfloat density) { engineHost().setDensity(density); }

JNIEXPORT jstring JNICALL
Java_com_glammer_mge_NativeEngine_stats(JNIEnv* env, jobject) {
    return env->NewStringUTF(engineHost().statsText().c_str());
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
