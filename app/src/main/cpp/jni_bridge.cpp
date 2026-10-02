#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <string>
#include "input_injector.h"

#define LOG_TAG "SGRE_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static ANativeWindow* gNativeWindow = nullptr;
static std::string gDataDirectory = "";

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeInit(
    JNIEnv* env,
    jclass /* clazz */,
    jobject surface,
    jstring dataPath) {

    if (gNativeWindow != nullptr) {
        ANativeWindow_release(gNativeWindow);
        gNativeWindow = nullptr;
    }

    gNativeWindow = ANativeWindow_fromSurface(env, surface);

    const char* pathStr = env->GetStringUTFChars(dataPath, nullptr);
    if (pathStr != nullptr) {
        gDataDirectory = pathStr;
        env->ReleaseStringUTFChars(dataPath, pathStr);
    }

    LOGI("Native Init complete with data directory: %s", gDataDirectory.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeSendTouchEvent(
    JNIEnv* /* env */,
    jclass /* clazz */,
    jint action,
    jfloat normalizedX,
    jfloat normalizedY) {

    InputInjector::injectTouchEvent(static_cast<GameTouchAction>(action), normalizedX, normalizedY);
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativePause(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    LOGI("Game loop paused");
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeResume(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    LOGI("Game loop resumed");
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeDestroy(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    if (gNativeWindow != nullptr) {
        ANativeWindow_release(gNativeWindow);
        gNativeWindow = nullptr;
    }
    LOGI("Native Window released and destroyed");
}
