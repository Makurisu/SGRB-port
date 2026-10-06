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
#include "win32_bridge.h"
#include "d3d11_bridge.h"

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
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(display, attribs, &config, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("eglChooseConfig thất bại");
        return;
    }

    EGLint visualId = 0;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &visualId);
    if (gNativeWindow != nullptr) {
        ANativeWindow_setBuffersGeometry(gNativeWindow, 0, 0, visualId);
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

    LOGI("EGL Context đã sẵn sàng, thiết lập Shader Program GLES 2.0 cho Quad Rendering...");

    const char* vertexShaderSource =
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "    vTexCoord = aTexCoord;\n"
        "}\n";

    const char* fragmentShaderSource =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
        "}\n";

    auto compileShader = [](GLenum type, const char* source) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen > 0) {
                std::string infoLog(infoLen, '\0');
                glGetShaderInfoLog(shader, infoLen, nullptr, &infoLog[0]);
                LOGE("Lỗi biên dịch shader (%u): %s", type, infoLog.c_str());
            }
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vertShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOGE("glLinkProgram thất bại");
    }

    GLint aPositionLoc = glGetAttribLocation(program, "aPosition");
    GLint aTexCoordLoc = glGetAttribLocation(program, "aTexCoord");
    GLint uTextureLoc = glGetUniformLocation(program, "uTexture");

    // Quad Vertex Data: [X, Y, U, V] (Full-screen quad, lật trục V để hiển thị đúng chiều)
    const float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
    };

    GLuint quadTexture = 0;
    glGenTextures(1, &quadTexture);
    glBindTexture(GL_TEXTURE_2D, quadTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    uint32_t currentTexWidth = 0;
    uint32_t currentTexHeight = 0;

    LOGI("Bắt đầu vòng lặp Render đồ hoạ đồng bộ theo D3D11 SwapChain...");

    RenderFrameState frameState = {};
    float hue = 0.0f;
    while (gIsRunning.load()) {
        bool hasFrame = D3D11Bridge_WaitForNextFrame(&frameState, 33);
        if (hasFrame) {
            glClearColor(
                frameState.clearColor[0],
                frameState.clearColor[1],
                frameState.clearColor[2],
                frameState.clearColor[3]
            );
        } else {
            // Khi chưa nhận frame mới từ D3D11, duy trì chu kỳ hiển thị nhẹ nhàng
            hue += 0.01f;
            if (hue > 1.0f) hue = 0.0f;
            float r = 0.15f + 0.1f * hue;
            float g = 0.08f;
            float b = 0.04f;
            glClearColor(r, g, b, 1.0f);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Kiểm tra xem có texture game RGBA8 hợp lệ đang hoạt động để render quad hay không
        uint32_t activeW = 0, activeH = 0;
        const uint8_t* activePixels = nullptr;
        bool hasActiveTexture = D3D11Bridge_GetActiveTextureInfo(&activeW, &activeH, &activePixels);

        if (hasActiveTexture && activePixels != nullptr && activeW > 0 && activeH > 0) {
            glUseProgram(program);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, quadTexture);

            if (currentTexWidth != activeW || currentTexHeight != activeH) {
                currentTexWidth = activeW;
                currentTexHeight = activeH;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, activeW, activeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, activePixels);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, activeW, activeH, GL_RGBA, GL_UNSIGNED_BYTE, activePixels);
            }

            glUniform1i(uTextureLoc, 0);

            glEnableVertexAttribArray(aPositionLoc);
            glVertexAttribPointer(aPositionLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &quadVertices[0]);

            glEnableVertexAttribArray(aTexCoordLoc);
            glVertexAttribPointer(aTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &quadVertices[2]);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(aPositionLoc);
            glDisableVertexAttribArray(aTexCoordLoc);
        }

        eglSwapBuffers(display, surface);

        static uint32_t sPresentCounter = 0;
        if (hasFrame && ++sPresentCounter % 60 == 1) {
            LOGI("EGL Present: Frame %u (%ux%u) | ActiveTex: %dx%d (%s) | Color: (%.2f, %.2f, %.2f) | DrawIndexed: %u | Textures: %u | Shaders: %u",
                 frameState.frameIndex, frameState.width, frameState.height,
                 activeW, activeH, hasActiveTexture ? "BOUND" : "NONE",
                 frameState.clearColor[0], frameState.clearColor[1], frameState.clearColor[2],
                 frameState.indexDraws, frameState.texturesCreated, frameState.shadersCreated);
        }
    }

    if (quadTexture != 0) {
        glDeleteTextures(1, &quadTexture);
    }
    if (program != 0) {
        glDeleteProgram(program);
    }
    if (vertShader != 0) {
        glDeleteShader(vertShader);
    }
    if (fragShader != 0) {
        glDeleteShader(fragShader);
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
    Win32Bridge_SetDataDirectory(gDataDirectory.c_str());

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
            Win32Bridge_SetLoadedImageBase(loadedBase);
            bool relocSuccess = PayloadLoader::applyRelocations(loadedBase, payloadInfo);
            bool importSuccess = PayloadLoader::resolveImports(loadedBase, payloadInfo);
            bool protectSuccess = PayloadLoader::protectSections(loadedBase, payloadInfo);
            LOGI("Ảo hoá bộ nhớ, Reloc (%s), IAT (%s), PageProt (%s) thành công! EntryPoint: %p, Tổng DLL: %zu",
                 relocSuccess ? "PASS" : "FAIL",
                 importSuccess ? "PASS" : "FAIL",
                 protectSuccess ? "PASS" : "FAIL",
                 static_cast<uint8_t*>(loadedBase) + payloadInfo.entryPointRva,
                 payloadInfo.importedDlls.size());

            // Chạy Game EntryPoint trên một luồng Worker riêng biệt để không block giao diện
            std::thread gameThread([loadedBase, payloadInfo]() {
                PayloadLoader::executePayload(loadedBase, payloadInfo);
            });
            gameThread.detach();
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

