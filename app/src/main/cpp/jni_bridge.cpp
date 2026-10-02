#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include "input_injector.h"
#include "payload_loader.h"

#define LOG_TAG "SGRE_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ANativeWindow* gNativeWindow = nullptr;
static std::string gDataDirectory = "";

static std::atomic<bool> gIsRunning{false};
static std::thread gRenderThread;

static void renderLoop() {
    LOGI("Bắt đầu khởi tạo EGL Rendering Context...");

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay thất bại");
        return;
    }

    if (!eglInitialize(display, nullptr, nullptr)) {
        LOGE("eglInitialize thất bại");
        return;
    }

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(display, attribs, &config, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("eglChooseConfig thất bại");
        return;
    }

    EGLSurface surface = eglCreateWindowSurface(display, config, gNativeWindow, nullptr);
    if (surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface thất bại");
        return;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext thất bại");
        return;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        LOGE("eglMakeCurrent thất bại");
        return;
    }

    LOGI("EGL Context đã sẵn sàng, bắt đầu vòng lặp Render đồ hoạ...");

    float hue = 0.0f;
    while (gIsRunning.load()) {
        // Màu khởi động ấm Steins;Gate nâu/cam cổ điển (Retro SciADV theme)
        hue += 0.01f;
        if (hue > 1.0f) hue = 0.0f;

        float r = 0.15f + 0.1f * hue;
        float g = 0.08f;
        float b = 0.04f;

        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        eglSwapBuffers(display, surface);
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    LOGI("Dừng vòng lặp render, giải phóng EGL...");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeInit(
    JNIEnv* env,
    jclass /* clazz */,
    jobject surface,
    jstring dataPath) {

    if (gIsRunning.load()) {
        gIsRunning.store(false);
        if (gRenderThread.joinable()) {
            gRenderThread.join();
        }
    }

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

    LOGI("Native Init hoàn tất, data dir: %s", gDataDirectory.c_str());

    GamePayloadInfo payloadInfo = PayloadLoader::inspectPayload(gDataDirectory);
    if (payloadInfo.isExeValid) {
        LOGI("Game Executable hợp lệ! Size: %llu bytes, Machine: 0x%X, Sections: %u, EntryPoint: 0x%X, ImageBase: 0x%llX",
             static_cast<unsigned long long>(payloadInfo.exeSize),
             payloadInfo.machineType,
             payloadInfo.numberOfSections,
             payloadInfo.entryPointRva,
             static_cast<unsigned long long>(payloadInfo.imageBase));

        void* loadedBase = PayloadLoader::mapImageToMemory(payloadInfo);
        if (loadedBase != nullptr) {
            bool relocSuccess = PayloadLoader::applyRelocations(loadedBase, payloadInfo);
            bool importSuccess = PayloadLoader::resolveImports(loadedBase, payloadInfo);
            bool protectSuccess = PayloadLoader::protectSections(loadedBase, payloadInfo);
            LOGI("Ảo hoá bộ nhớ, Reloc (%s), IAT (%s), PageProt (%s) thành công! EntryPoint: %p, Tổng DLL: %zu",
                 relocSuccess ? "PASS" : "FAIL",
                 importSuccess ? "PASS" : "FAIL",
                 protectSuccess ? "PASS" : "FAIL",
                 static_cast<uint8_t*>(loadedBase) + payloadInfo.entryPointRva,
                 payloadInfo.importedDlls.size());
        }
    } else {
        LOGE("Chưa tìm thấy hoặc binary sgre_steam.exe không hợp lệ");
    }

    if (gNativeWindow != nullptr) {
        gIsRunning.store(true);
        gRenderThread = std::thread(renderLoop);
    }
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

    if (gIsRunning.load()) {
        gIsRunning.store(false);
        if (gRenderThread.joinable()) {
            gRenderThread.join();
        }
    }

    if (gNativeWindow != nullptr) {
        ANativeWindow_release(gNativeWindow);
        gNativeWindow = nullptr;
    }
    LOGI("Native Window và tài nguyên render đã giải phóng");
}

