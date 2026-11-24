#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cmath>
#include <cstring>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include "input_injector.h"
#include "payload_loader.h"
#include "win32_bridge.h"
#include "d3d11_bridge.h"
#include "shader_cache.h"
#include "steam_hook.h"
#include "audio_sink.h"
#include "xaudio2_bridge.h"
#include "xinput_bridge.h"
#include "arm64_box64_bridge.h"

#define LOG_TAG "SGRE_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ANativeWindow* gNativeWindow = nullptr;
static std::string gDataDirectory = "";

static std::atomic<bool> gIsRunning{false};
static std::atomic<bool> gEngineLaunched{false};
static std::atomic<bool> gMaintainAspectRatio16x9{true};
static std::atomic<int32_t> gTargetFps{60};
static std::atomic<float> gCurrentFps{0.0f};
static std::mutex gLifecycleMutex;
static std::thread gRenderThread;

static void computeActiveSurfaceRect(
    EGLint surfW, EGLint surfH, bool maintain16x9,
    GLint& outX, GLint& outY, GLsizei& outW, GLsizei& outH
) {
    if (surfW <= 0 || surfH <= 0) {
        outX = 0;
        outY = 0;
        outW = 1920;
        outH = 1080;
        return;
    }
    if (!maintain16x9) {
        outX = 0;
        outY = 0;
        outW = surfW;
        outH = surfH;
        return;
    }
    const int64_t lhs = static_cast<int64_t>(surfW) * 9ll;
    const int64_t rhs = static_cast<int64_t>(surfH) * 16ll;
    if (lhs > rhs) {
        // Màn hình dài hơn 16:9 (VD: 20:9, 21:9) -> Pillarbox hai cạnh trái/phải
        outH = surfH;
        outW = std::max<GLsizei>(1, static_cast<GLsizei>((rhs + 4ll) / 9ll));
        outX = (surfW - outW) / 2;
        outY = 0;
    } else if (lhs < rhs) {
        // Màn hình vuông hơn 16:9 (VD: 4:3, màn gập) -> Letterbox cạnh trên/dưới
        outW = surfW;
        outH = std::max<GLsizei>(1, static_cast<GLsizei>((lhs + 8ll) / 16ll));
        outX = 0;
        outY = (surfH - outH) / 2;
    } else {
        outX = 0;
        outY = 0;
        outW = surfW;
        outH = surfH;
    }
}

// Bảng ánh xạ RenderTarget / Texture GPU handle sang Framebuffer Object (FBO) OpenGL ES
static std::unordered_map<const void*, GLuint> sFboCache;
static std::unordered_map<const void*, GLuint> sCopyFboCache;
static std::unordered_map<const void*, GLuint> sStencilRboCache;
static std::unordered_set<const void*> sRenderTargetHandles;
static std::unordered_set<const void*> sGpuCopiedHandles;

struct RecycledGlesResource {
    GLuint texId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;
    GLuint fbo = 0;
    GLuint copyFbo = 0;
    GLuint stencilRbo = 0;
    bool hasRtFbo = false;
};

static std::vector<RecycledGlesResource> sRecyclePool;
static uint64_t sRecyclePoolBytes = 0;
constexpr size_t kMaxRecyclePoolEntries = 16;
constexpr uint64_t kMaxRecyclePoolBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxSinglePooledTexBytes = 16ull * 1024 * 1024;
constexpr GLenum kGlCompressedRgbaBptcUnormExt = 0x8E8Cu;
constexpr float kPerfTraceThresholdMs = 25.0f;

static bool checkExtensionSupported(const char* extName) {
    if (!extName || extName[0] == '\0') return false;
    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    if (numExt > 0) {
        for (GLint i = 0; i < numExt; ++i) {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (ext && std::strcmp(ext, extName) == 0) {
                return true;
            }
        }
    }
    const char* extStr = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!extStr) return false;
    const size_t targetLen = std::strlen(extName);
    const char* cur = extStr;
    while ((cur = std::strstr(cur, extName)) != nullptr) {
        const bool startOk = (cur == extStr) || (cur[-1] == ' ');
        const char endCh = cur[targetLen];
        if (startOk && (endCh == '\0' || endCh == ' ')) {
            return true;
        }
        cur += targetLen;
    }
    return false;
}

static void destroyRecycledResource(RecycledGlesResource& res) {
    if (res.fbo != 0) glDeleteFramebuffers(1, &res.fbo);
    if (res.copyFbo != 0) glDeleteFramebuffers(1, &res.copyFbo);
    if (res.stencilRbo != 0) glDeleteRenderbuffers(1, &res.stencilRbo);
    if (res.texId != 0) glDeleteTextures(1, &res.texId);
    res = {};
}

static void clearRecyclePool() {
    for (auto& res : sRecyclePool) {
        destroyRecycledResource(res);
    }
    sRecyclePool.clear();
    sRecyclePoolBytes = 0;
}

static void pushToRecyclePool(RecycledGlesResource&& res) {
    if (res.texId == 0 || res.bytes == 0 || res.bytes > kMaxSinglePooledTexBytes ||
        res.width < 64 || res.height < 64) {
        destroyRecycledResource(res);
        return;
    }
    while (sRecyclePool.size() >= kMaxRecyclePoolEntries ||
           (sRecyclePoolBytes + res.bytes > kMaxRecyclePoolBytes && !sRecyclePool.empty())) {
        RecycledGlesResource oldest = sRecyclePool.front();
        sRecyclePool.erase(sRecyclePool.begin());
        sRecyclePoolBytes -= oldest.bytes;
        destroyRecycledResource(oldest);
    }
    if (sRecyclePoolBytes + res.bytes > kMaxRecyclePoolBytes) {
        destroyRecycledResource(res);
        return;
    }
    sRecyclePoolBytes += res.bytes;
    sRecyclePool.push_back(std::move(res));
}

static bool popFromRecyclePool(uint32_t w, uint32_t h, bool needRtFbo, RecycledGlesResource& out) {
    int bestIdx = -1;
    for (size_t i = 0; i < sRecyclePool.size(); ++i) {
        const auto& item = sRecyclePool[i];
        if (item.width == w && item.height == h) {
            if (item.hasRtFbo == needRtFbo) {
                bestIdx = static_cast<int>(i);
                break;
            }
            if (bestIdx < 0 && !needRtFbo && !item.hasRtFbo) {
                bestIdx = static_cast<int>(i);
            } else if (bestIdx < 0 && needRtFbo && item.fbo == 0) {
                bestIdx = static_cast<int>(i);
            }
        }
    }
    if (bestIdx < 0) return false;
    out = sRecyclePool[bestIdx];
    sRecyclePool.erase(sRecyclePool.begin() + bestIdx);
    sRecyclePoolBytes -= out.bytes;
    return true;
}

static void releaseHandleFromRenderThreadCaches(const void* handle) {
    auto itFbo = sFboCache.find(handle);
    if (itFbo != sFboCache.end()) {
        if (itFbo->second != 0) glDeleteFramebuffers(1, &itFbo->second);
        sFboCache.erase(itFbo);
    }
    auto itCopyFbo = sCopyFboCache.find(handle);
    if (itCopyFbo != sCopyFboCache.end()) {
        if (itCopyFbo->second != 0) glDeleteFramebuffers(1, &itCopyFbo->second);
        sCopyFboCache.erase(itCopyFbo);
    }
    auto itRbo = sStencilRboCache.find(handle);
    if (itRbo != sStencilRboCache.end()) {
        if (itRbo->second != 0) glDeleteRenderbuffers(1, &itRbo->second);
        sStencilRboCache.erase(itRbo);
    }
    sRenderTargetHandles.erase(handle);
    sGpuCopiedHandles.erase(handle);
}

static void swapRgbaToBgraRow(const uint8_t* srcRow, uint8_t* dstRow, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* s = srcRow + static_cast<size_t>(x) * 4;
        uint8_t* d = dstRow + static_cast<size_t>(x) * 4;
        d[0] = s[2]; // B
        d[1] = s[1]; // G
        d[2] = s[0]; // R
        d[3] = s[3]; // A
    }
}

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
        EGL_STENCIL_SIZE, 8,
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

    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        LOGI("eglCreateContext(GLES 3) không thành công, thử tạo context GLES 2...");
        contextAttribs[1] = 2;
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    }
    if (context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext thất bại");
        return;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        LOGE("eglMakeCurrent thất bại");
        return;
    }

    // Thiết lập khoảng đồng bộ VSync theo mục tiêu FPS người dùng cấu hình (60 FPS -> 1, 30 FPS -> 2)
    int32_t appliedTargetFps = gTargetFps.load(std::memory_order_relaxed);
    eglSwapInterval(display, appliedTargetFps <= 30 ? 2 : 1);

    EGLint glClientVersion = 2;
    eglQueryContext(display, context, EGL_CONTEXT_CLIENT_VERSION, &glClientVersion);
    const bool isGles3Context = (glClientVersion >= 3);
    const bool bptcExtSupported = isGles3Context &&
        (checkExtensionSupported("GL_EXT_texture_compression_bptc") ||
         checkExtensionSupported("GL_ARB_texture_compression_bptc"));
    D3D11Bridge_SetSupportsBptcCompression(bptcExtSupported);
    LOGI("EGL Context đã sẵn sàng (GLES %d), BPTC GPU BC7=%s, thiết lập Shader Program cho Quad Rendering...",
         glClientVersion, bptcExtSupported ? "YES" : "NO");

    const char* vertexShaderSource =
        "attribute vec3 aPosition;\n"
        "attribute vec2 aTexCoord0;\n"
        "attribute vec2 aTexCoord1;\n"
        "attribute vec4 aColor;\n"
        "varying vec2 vTexCoord;\n"
        "varying vec2 vTexCoord1;\n"
        "varying vec4 vColor;\n"
        "uniform int uVsShaderId;\n"
        "uniform vec4 uVsCb0[16];\n"
        "uniform vec2 uUvScale;\n"
        "uniform float uFlipY;\n"
        "void main() {\n"
        "    vec4 pos;\n"
        "    if (uVsShaderId < 6) {\n"
        "        vec3 r0 = vec3(aPosition.xy, 1.0);\n"
        "        pos = vec4(\n"
        "            dot(r0, uVsCb0[0].xyw),\n"
        "            dot(r0, uVsCb0[1].xyw),\n"
        "            dot(r0, uVsCb0[2].xyw),\n"
        "            dot(r0, uVsCb0[3].xyw)\n"
        "        );\n"
        "    } else {\n"
        "        vec4 r0 = vec4(aPosition.xyz, 1.0);\n"
        "        pos = vec4(\n"
        "            dot(r0, uVsCb0[0].xyzw),\n"
        "            dot(r0, uVsCb0[1].xyzw),\n"
        "            dot(r0, uVsCb0[2].xyzw),\n"
        "            dot(r0, uVsCb0[3].xyzw)\n"
        "        );\n"
        "    }\n"
        "    if (pos.w == 0.0) {\n"
        "        pos.w = 1.0;\n"
        "    }\n"
        "    gl_Position = vec4(pos.x, pos.y * uFlipY, pos.z, pos.w);\n"
        "    vColor = aColor;\n"
        "    vec2 uv0 = aTexCoord0 * uUvScale;\n"
        "    vec2 uv1 = aTexCoord1;\n"
        "    if (uVsShaderId == 0 || uVsShaderId == 1 || uVsShaderId == 6 || uVsShaderId == 7) {\n"
        "        vTexCoord = vec2(0.0);\n"
        "        vTexCoord1 = vec2(0.0);\n"
        "    } else if (uVsShaderId == 2 || uVsShaderId == 4 || uVsShaderId == 8 || uVsShaderId == 10) {\n"
        "        vTexCoord = uv0;\n"
        "        vTexCoord1 = vec2(0.0);\n"
        "    } else {\n"
        "        vTexCoord = uv0;\n"
        "        vTexCoord1 = uv1;\n"
        "    }\n"
        "}\n";

    const char* fragmentShaderSource =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 vTexCoord;\n"
        "varying vec2 vTexCoord1;\n"
        "varying vec4 vColor;\n"
        "uniform sampler2D uTexture;\n"
        "uniform sampler2D uAlphaTexture;\n"
        "uniform int uHasAlphaTexture;\n"
        "uniform sampler2D uDestTexture;\n"
        "uniform int uHasDestTexture;\n"
        "uniform int uShaderId;\n"
        "uniform vec4 uPsCb0[8];\n"
        "uniform float uFlipY;\n"
        "uniform vec4 uD3dViewport;\n"
        "uniform vec4 uGlViewport;\n"
        "void main() {\n"
        "    vec4 baseColor = texture2D(uTexture, vTexCoord);\n"
        "    vec4 uCb0 = uPsCb0[0];\n"
        "    vec2 normPos = vec2(\n"
        "        (gl_FragCoord.x - uGlViewport.x) / uGlViewport.z,\n"
        "        (uFlipY > 0.0) ? (1.0 - (gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
        "                       : ((gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
        "    );\n"
        "    vec2 svPos = uD3dViewport.xy + normPos * uD3dViewport.zw;\n"
        "    if (uShaderId < 900) {\n"
        "        int colorMode = uShaderId / 60;\n"
        "        int rem = uShaderId - colorMode * 60;\n"
        "        int maskMode = rem / 15;\n"
        "        int rem15 = rem - maskMode * 15;\n"
        "        int filterMode = rem15 / 3;\n"
        "        int outMode = rem15 - filterMode * 3;\n"
        "        vec4 c;\n"
        "        if (colorMode == 0) {\n"
        "            c = vColor;\n"
        "        } else if (colorMode == 1) {\n"
        "            c = vColor * vColor;\n"
        "        } else if (colorMode == 2) {\n"
        "            vec4 v2 = vColor * vColor;\n"
        "            c = vec4(v2.rgb * 2.0, v2.a);\n"
        "        } else if (colorMode == 3) {\n"
        "            c = baseColor;\n"
        "        } else if (colorMode == 4) {\n"
        "            c = baseColor * vColor;\n"
        "        } else if (colorMode == 5) {\n"
        "            vec4 m = baseColor * vColor;\n"
        "            c = vec4(clamp(m.rgb * 2.0, 0.0, 1.0), m.a);\n"
        "        } else if (colorMode == 6) {\n"
        "            c = vec4(1.0, 1.0, 1.0, baseColor.a);\n"
        "        } else if (colorMode == 7) {\n"
        "            c = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
        "        } else if (colorMode == 8) {\n"
        "            vec4 m = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        } else if (colorMode == 9) {\n"
        "            c = vec4(baseColor.rrr, 1.0);\n"
        "        } else if (colorMode == 10) {\n"
        "            c = vec4(baseColor.rrr, 1.0) * vColor;\n"
        "        } else if (colorMode == 11) {\n"
        "            vec4 m = vec4(baseColor.rrr, 1.0) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        } else if (colorMode == 12) {\n"
        "            c = vec4(baseColor.rrr, baseColor.g);\n"
        "        } else if (colorMode == 13) {\n"
        "            c = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
        "        } else {\n"
        "            vec4 m = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        }\n"
        "        vec4 fCol = (maskMode == 1) ? uPsCb0[1] : uPsCb0[0];\n"
        "        if (filterMode == 1) {\n"
        "            c.rgb = mix(c.rgb, fCol.rgb, fCol.a);\n"
        "        } else if (filterMode == 2) {\n"
        "            c.rgb = c.rgb + fCol.rgb * fCol.a;\n"
        "        } else if (filterMode == 3) {\n"
        "            c.rgb = c.rgb - fCol.rgb * fCol.a;\n"
        "        } else if (filterMode == 4) {\n"
        "            c.rgb = c.rgb + (vec3(1.0) - 2.0 * c.rgb) * fCol.a;\n"
        "        }\n"
        "        if (uHasAlphaTexture == 1) {\n"
        "            if (maskMode == 1) {\n"
        "                vec2 maskUv = (uPsCb0[0].x > 0.0 && uPsCb0[0].y > 0.0) ? (svPos * uPsCb0[0].xy) : normPos;\n"
        "                c.a *= texture2D(uAlphaTexture, maskUv).a;\n"
        "            } else if (maskMode == 2) {\n"
        "                float ma = texture2D(uAlphaTexture, vTexCoord).a;\n"
        "                if (c.a * ma < 0.001961) discard;\n"
        "                c.a *= ma;\n"
        "            } else if (maskMode == 3) {\n"
        "                c.a *= texture2D(uAlphaTexture, vTexCoord1).a;\n"
        "            }\n"
        "        }\n"
        "        if (outMode == 1) {\n"
        "            c.rgb = vec3(1.0) - c.rgb;\n"
        "        } else if (outMode == 2) {\n"
        "            c.rgb *= c.a;\n"
        "        }\n"
        "        gl_FragColor = c;\n"
        "    } else if (uShaderId == 2040) {\n"
        "        float maskVal = (uHasAlphaTexture == 1) ? texture2D(uAlphaTexture, vTexCoord1).r : 1.0;\n"
        "        float phase = uCb0.x;\n"
        "        float invVague = uCb0.y;\n"
        "        float effAlpha = clamp((maskVal - phase) * invVague + 1.0, 0.0, 1.0) * vColor.a;\n"
        "        gl_FragColor = vec4(baseColor.rgb, baseColor.a * effAlpha);\n"
        "    } else if (uShaderId == 2041) {\n"
        "        vec4 maskSample = texture2D(uTexture, vTexCoord1);\n"
        "        gl_FragColor = vec4(baseColor.rgb * vColor.rgb, maskSample.r * vColor.a);\n"
        "    } else if (uShaderId >= 2042 && uShaderId <= 2071) {\n"
        "        int idx = uShaderId - 2042;\n"
        "        int grp = idx / 6;\n"
        "        int sub = idx - grp * 6;\n"
        "        vec3 outRgb;\n"
        "        float rawA;\n"
        "        if (grp == 0) {\n"
        "            outRgb = vColor.rgb;\n"
        "            rawA = vColor.a;\n"
        "        } else if (grp == 1) {\n"
        "            outRgb = baseColor.rgb;\n"
        "            rawA = baseColor.a;\n"
        "        } else if (grp == 2) {\n"
        "            outRgb = vec3(1.0);\n"
        "            rawA = baseColor.a;\n"
        "        } else if (grp == 3) {\n"
        "            outRgb = baseColor.rrr;\n"
        "            rawA = 1.0;\n"
        "        } else {\n"
        "            outRgb = baseColor.rrr;\n"
        "            rawA = baseColor.g;\n"
        "        }\n"
        "        float effAlpha = (sub >= 3) ? clamp(rawA * uCb0.x + uCb0.y, 0.0, 1.0) : rawA;\n"
        "        if ((sub == 1 || sub == 4) && uHasAlphaTexture == 1) {\n"
        "            vec2 maskUv = (uPsCb0[1].x > 0.0 && uPsCb0[1].y > 0.0) ? (svPos * uPsCb0[1].xy) : normPos;\n"
        "            effAlpha *= texture2D(uAlphaTexture, maskUv).a;\n"
        "        }\n"
        "        gl_FragColor = vec4(outRgb, effAlpha);\n"
        "    } else if (uShaderId == 2072) {\n"
        "        if (uCb0.x >= vColor.a) discard;\n"
        "        gl_FragColor = vColor;\n"
        "    } else if (uShaderId == 2073) {\n"
        "        if (uCb0.x >= baseColor.a) discard;\n"
        "        gl_FragColor = baseColor;\n"
        "    } else if (uShaderId == 2074) {\n"
        "        if (uCb0.x >= baseColor.a) discard;\n"
        "        gl_FragColor = vec4(1.0, 1.0, 1.0, baseColor.a);\n"
        "    } else if (uShaderId == 2075) {\n"
        "        if (uCb0.x >= 1.0) discard;\n"
        "        gl_FragColor = vec4(baseColor.rrr, 1.0);\n"
        "    } else if (uShaderId == 2076) {\n"
        "        if (uCb0.x >= baseColor.g) discard;\n"
        "        gl_FragColor = vec4(baseColor.rrr, baseColor.g);\n"
        "    } else if (uShaderId >= 900 && uShaderId < 2040) {\n"
        "        int idx = uShaderId - 900;\n"
        "        int rem19 = idx / 19;\n"
        "        int blendMode = idx - rem19 * 19;\n"
        "        int colorMode = rem19 / 4;\n"
        "        int maskMode = rem19 - colorMode * 4;\n"
        "        vec4 c;\n"
        "        if (colorMode == 0) {\n"
        "            c = vColor;\n"
        "        } else if (colorMode == 1) {\n"
        "            c = vColor * vColor;\n"
        "        } else if (colorMode == 2) {\n"
        "            vec4 v2 = vColor * vColor;\n"
        "            c = vec4(v2.rgb * 2.0, v2.a);\n"
        "        } else if (colorMode == 3) {\n"
        "            c = baseColor;\n"
        "        } else if (colorMode == 4) {\n"
        "            c = baseColor * vColor;\n"
        "        } else if (colorMode == 5) {\n"
        "            vec4 m = baseColor * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        } else if (colorMode == 6) {\n"
        "            c = vec4(1.0, 1.0, 1.0, baseColor.a);\n"
        "        } else if (colorMode == 7) {\n"
        "            c = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
        "        } else if (colorMode == 8) {\n"
        "            vec4 m = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        } else if (colorMode == 9) {\n"
        "            c = vec4(baseColor.rrr, 1.0);\n"
        "        } else if (colorMode == 10) {\n"
        "            c = vec4(baseColor.rrr, 1.0) * vColor;\n"
        "        } else if (colorMode == 11) {\n"
        "            vec4 m = vec4(baseColor.rrr, 1.0) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        } else if (colorMode == 12) {\n"
        "            c = vec4(baseColor.rrr, baseColor.g);\n"
        "        } else if (colorMode == 13) {\n"
        "            c = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
        "        } else {\n"
        "            vec4 m = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
        "            c = vec4(m.rgb * 2.0, m.a);\n"
        "        }\n"
        "        if (uHasAlphaTexture == 1) {\n"
        "            if (maskMode == 1) {\n"
        "                vec2 maskUv = (uPsCb0[0].x > 0.0 && uPsCb0[0].y > 0.0) ? (svPos * uPsCb0[0].xy) : normPos;\n"
        "                c.a *= texture2D(uAlphaTexture, maskUv).a;\n"
        "            } else if (maskMode == 2) {\n"
        "                float ma = texture2D(uAlphaTexture, vTexCoord).a;\n"
        "                if (c.a * ma < 0.001961) discard;\n"
        "                c.a *= ma;\n"
        "            } else if (maskMode == 3) {\n"
        "                c.a *= texture2D(uAlphaTexture, vTexCoord1).a;\n"
        "            }\n"
        "        }\n"
        "        if (blendMode == 0 || uHasDestTexture == 0) {\n"
        "            gl_FragColor = c;\n"
        "        } else {\n"
        "            vec4 smo = (maskMode == 1) ? uPsCb0[1] : uPsCb0[0];\n"
        "            vec4 bp = (maskMode == 1) ? uPsCb0[2] : uPsCb0[1];\n"
        "            vec2 destUv = svPos * smo.xy + smo.zw;\n"
        "            vec4 dst = texture2D(uDestTexture, destUv);\n"
        "            vec3 b = dst.rgb;\n"
        "            if (blendMode == 1) {\n"
        "                b = min(dst.rgb + c.rgb, vec3(1.0));\n"
        "            } else if (blendMode == 2) {\n"
        "                b = max(dst.rgb + c.rgb - vec3(1.0), vec3(0.0));\n"
        "            } else if (blendMode == 3) {\n"
        "                vec3 lo = 2.0 * dst.rgb * c.rgb;\n"
        "                vec3 hi = vec3(1.0) - 2.0 * (vec3(1.0) - dst.rgb) * (vec3(1.0) - c.rgb);\n"
        "                b = mix(hi, lo, step(dst.rgb, vec3(0.5)));\n"
        "            } else if (blendMode == 4) {\n"
        "                vec3 lo = 2.0 * dst.rgb * c.rgb;\n"
        "                vec3 hi = vec3(1.0) - 2.0 * (vec3(1.0) - dst.rgb) * (vec3(1.0) - c.rgb);\n"
        "                b = mix(hi, lo, step(c.rgb, vec3(0.5)));\n"
        "            } else if (blendMode == 5) {\n"
        "                vec3 expVal = mix(2.0 * (vec3(1.0) - c.rgb), vec3(0.5) / max(c.rgb, vec3(0.0001)), step(vec3(0.5), c.rgb));\n"
        "                b = pow(max(dst.rgb, vec3(0.0)), max(expVal, vec3(0.0)));\n"
        "            } else if (blendMode == 6) {\n"
        "                b = min(dst.rgb / max(vec3(1.0) - c.rgb, vec3(0.0001)), vec3(1.0));\n"
        "            } else if (blendMode == 7) {\n"
        "                b = max(vec3(1.0) - (vec3(1.0) - dst.rgb) / max(c.rgb, vec3(0.0001)), vec3(0.0));\n"
        "            } else if (blendMode == 8) {\n"
        "                b = max(dst.rgb, c.rgb);\n"
        "            } else if (blendMode == 9) {\n"
        "                b = min(dst.rgb, c.rgb);\n"
        "            } else if (blendMode == 10) {\n"
        "                b = abs(dst.rgb - c.rgb);\n"
        "            } else if (blendMode == 11) {\n"
        "                b = dst.rgb + c.rgb - 2.0 * dst.rgb * c.rgb;\n"
        "            } else if (blendMode == 12) {\n"
        "                float lum = dot(dst.rgb, vec3(0.298912, 0.586611, 0.114478));\n"
        "                b = lum * c.rgb;\n"
        "            } else if (blendMode == 13) {\n"
        "                vec2 mosUv = floor(destUv * max(bp.xy, vec2(0.0001))) / max(bp.xy, vec2(0.0001)) + bp.zw;\n"
        "                b = texture2D(uDestTexture, mosUv).rgb;\n"
        "            } else if (blendMode == 14) {\n"
        "                b = 0.5 * (texture2D(uDestTexture, destUv - bp.xy * 0.5).rgb + texture2D(uDestTexture, destUv + bp.xy * 0.5).rgb);\n"
        "            } else if (blendMode == 15) {\n"
        "                b = 0.375 * dst.rgb + 0.3125 * (texture2D(uDestTexture, destUv + bp.xy * 1.2).rgb + texture2D(uDestTexture, destUv - bp.xy * 1.2).rgb);\n"
        "            } else if (blendMode == 16) {\n"
        "                b = 0.322581 * dst.rgb + 0.338710 * (texture2D(uDestTexture, destUv + bp.xy * 1.285714).rgb + texture2D(uDestTexture, destUv - bp.xy * 1.285714).rgb);\n"
        "            } else if (blendMode == 17) {\n"
        "                b = 0.227027 * dst.rgb + 0.316216 * (texture2D(uDestTexture, destUv + bp.xy * 1.384615).rgb + texture2D(uDestTexture, destUv - bp.xy * 1.384615).rgb) + 0.070270 * (texture2D(uDestTexture, destUv + bp.xy * 3.230769).rgb + texture2D(uDestTexture, destUv - bp.xy * 3.230769).rgb);\n"
        "            } else if (blendMode == 18) {\n"
        "                b = 0.185714 * dst.rgb + 0.288701 * (texture2D(uDestTexture, destUv + bp.xy * 1.421053).rgb + texture2D(uDestTexture, destUv - bp.xy * 1.421053).rgb) + 0.103636 * (texture2D(uDestTexture, destUv + bp.xy * 3.315789).rgb + texture2D(uDestTexture, destUv - bp.xy * 3.315789).rgb) + 0.014805 * (texture2D(uDestTexture, destUv + bp.xy * 5.210526).rgb + texture2D(uDestTexture, destUv - bp.xy * 5.210526).rgb);\n"
        "            }\n"
        "            gl_FragColor = vec4(mix(dst.rgb, b, c.a), c.a);\n"
        "        }\n"
        "    } else if (uHasAlphaTexture == 1) {\n"
        "        float maskAlpha = texture2D(uAlphaTexture, normPos).a;\n"
        "        gl_FragColor = vec4(baseColor.rgb, baseColor.a * maskAlpha) * vColor;\n"
        "    } else {\n"
        "        gl_FragColor = baseColor * vColor;\n"
        "    }\n"
        "}\n";

    sgre::ShaderProgramSet programSet = sgre::ShaderCache::buildProgramSet(
        gDataDirectory, vertexShaderSource, fragmentShaderSource, isGles3Context
    );
    const sgre::GlProgramBinding& uberBinding =
        programSet.families[static_cast<size_t>(sgre::ShaderFamily::UberFallback)];
    GLuint program = uberBinding.program;
    GLint aPositionLoc = uberBinding.aPositionLoc;
    GLint aTexCoord0Loc = uberBinding.aTexCoord0Loc;
    GLint aTexCoord1Loc = uberBinding.aTexCoord1Loc;
    GLint aColorLoc = uberBinding.aColorLoc;
    GLint uTextureLoc = uberBinding.uTextureLoc;
    GLint uAlphaTextureLoc = uberBinding.uAlphaTextureLoc;
    GLint uHasAlphaTextureLoc = uberBinding.uHasAlphaTextureLoc;
    GLint uDestTextureLoc = uberBinding.uDestTextureLoc;
    GLint uHasDestTextureLoc = uberBinding.uHasDestTextureLoc;
    GLint uVsShaderIdLoc = uberBinding.uVsShaderIdLoc;
    GLint uVsCb0Loc = uberBinding.uVsCb0Loc;
    GLint uShaderIdLoc = uberBinding.uShaderIdLoc;
    GLint uPsCb0Loc = uberBinding.uPsCb0Loc;
    GLint uUvScaleLoc = uberBinding.uUvScaleLoc;
    GLint uFlipYLoc = uberBinding.uFlipYLoc;
    GLint uD3dViewportLoc = uberBinding.uD3dViewportLoc;
    GLint uGlViewportLoc = uberBinding.uGlViewportLoc;

    LOGI("Shader Locations: aPos=%d, aTex0=%d, aTex1=%d, aCol=%d, uTex=%d, uVsId=%d, uVsCb0=%d, uUvScale=%d, uFlipY=%d, uShaderId=%d, uPsCb0=%d",
         aPositionLoc, aTexCoord0Loc, aTexCoord1Loc, aColorLoc, uTextureLoc, uVsShaderIdLoc, uVsCb0Loc, uUvScaleLoc, uFlipYLoc, uShaderIdLoc, uPsCb0Loc);

    glUseProgram(program);

    GLuint streamVbo = 0;
    GLuint streamIbo = 0;
    glGenBuffers(1, &streamVbo);
    glGenBuffers(1, &streamIbo);

    bool sTestedBackbufferBlit = false;
    bool sSupportsBackbufferBlit = isGles3Context;

    // Quad Vertex Data: 11 floats per vertex: [posX, posY, posZ, u0, v0, u1, v1, r, g, b, a]
    const float quadVertices[] = {
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 0.0f,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
    };

    // Texture 1x1 màu trắng bất biến (0xFFFFFFFF) dành riêng cho các mesh không gắn texture (untextured solid color)
    GLuint whitePixelTexture = 0;
    glGenTextures(1, &whitePixelTexture);
    glBindTexture(GL_TEXTURE_2D, whitePixelTexture);
    const uint32_t whitePixel = 0xFFFFFFFF;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Texture riêng dành cho nhánh fallback render quad toàn màn hình (không dùng chung với white pixel)
    GLuint fallbackQuadTexture = 0;
    glGenTextures(1, &fallbackQuadTexture);
    glBindTexture(GL_TEXTURE_2D, fallbackQuadTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    uint32_t currentTexWidth = 0;
    uint32_t currentTexHeight = 0;

    GLuint sBackbufferFlipTempTex = 0;
    uint32_t sBackbufferFlipTempW = 0;
    uint32_t sBackbufferFlipTempH = 0;

    struct CachedGlesTexture {
        GLuint texId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t revision = 0;
        uint64_t generationId = 0;
        bool isCompressedBc7 = false;
    };
    static std::unordered_map<const void*, CachedGlesTexture> sGlesTextureCache;

    struct ReadyPreuploadTexture {
        const void* handle = nullptr;
        GLuint texId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t revision = 0;
        uint64_t generationId = 0;
        bool isCompressedBc7 = false;
        GLsync fence = nullptr;
    };
    std::mutex readyPreuploadMutex;
    std::condition_variable preuploadDoneCv;
    std::vector<ReadyPreuploadTexture> readyPreuploads;
    std::unordered_set<const void*> inFlightPreuploads;
    std::vector<TextureUploadRequest> fallbackPreuploadPending;
    std::atomic<bool> preuploadRunning{true};
    std::atomic<bool> sharedUploaderActive{false};

    auto uploadBoundTexture2D = [&](const void* handle, uint32_t w, uint32_t h,
                                    const uint8_t* pixels, size_t pixelBytes,
                                    bool isCompressedBc7, bool useSubImage) -> bool {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (isCompressedBc7 && pixels != nullptr && pixelBytes > 0) {
            while (glGetError() != GL_NO_ERROR) {}
            glCompressedTexImage2D(
                GL_TEXTURE_2D, 0, kGlCompressedRgbaBptcUnormExt, w, h, 0,
                static_cast<GLsizei>(pixelBytes), pixels
            );
            GLenum compErr = glGetError();
            if (compErr == GL_NO_ERROR) {
                return true;
            }
            LOGW("glCompressedTexImage2D BPTC that bai (err=0x%X), tat BPTC va fallback decode CPU", compErr);
            D3D11Bridge_SetSupportsBptcCompression(false);
            if (handle != nullptr && D3D11Bridge_EnsureTextureDecompressed(handle)) {
                TextureCpuSnapshot decompSnap{};
                if (D3D11Bridge_AcquireTextureSnapshot(handle, decompSnap) &&
                    decompSnap.pixels && !decompSnap.pixels->empty() &&
                    !decompSnap.isCompressedBc7) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 decompSnap.pixels->data());
                    return false;
                }
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            return false;
        }
        if (useSubImage && pixels != nullptr) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        return false;
    };

    auto drainReadyPreuploads = [&]() {
        std::vector<ReadyPreuploadTexture> drained;
        {
            std::lock_guard<std::mutex> lock(readyPreuploadMutex);
            if (readyPreuploads.empty()) return;
            drained.swap(readyPreuploads);
        }
        for (const auto& item : drained) {
            if (item.fence != nullptr) {
                glWaitSync(item.fence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(item.fence);
            }
            auto itExisting = sGlesTextureCache.find(item.handle);
            if (itExisting != sGlesTextureCache.end() &&
                itExisting->second.generationId == item.generationId &&
                itExisting->second.revision == item.revision) {
                glDeleteTextures(1, &item.texId);
                continue;
            }
            TextureCpuSnapshot curSnap{};
            if (D3D11Bridge_AcquireTextureSnapshot(item.handle, curSnap) &&
                curSnap.generationId == item.generationId &&
                curSnap.revision == item.revision) {
                if (itExisting != sGlesTextureCache.end()) {
                    if (itExisting->second.texId != 0) glDeleteTextures(1, &itExisting->second.texId);
                    releaseHandleFromRenderThreadCaches(item.handle);
                }
                sGlesTextureCache[item.handle] = {
                    item.texId, item.width, item.height, item.revision, item.generationId, item.isCompressedBc7
                };
                D3D11Bridge_MarkTextureUploaded(item.handle);
            } else {
                glDeleteTextures(1, &item.texId);
            }
        }
    };

    auto releaseAllForHandle = [&](const void* handle) {
        auto itTex = sGlesTextureCache.find(handle);
        if (itTex != sGlesTextureCache.end()) {
            const uint64_t texBytes = static_cast<uint64_t>(itTex->second.width) * itTex->second.height * 4ull;
            if (!itTex->second.isCompressedBc7 &&
                itTex->second.texId != 0 && texBytes > 0 && texBytes <= kMaxSinglePooledTexBytes &&
                itTex->second.width >= 64 && itTex->second.height >= 64 &&
                sRenderTargetHandles.count(handle) != 0) {
                RecycledGlesResource rec{};
                rec.texId = itTex->second.texId;
                rec.width = itTex->second.width;
                rec.height = itTex->second.height;
                rec.bytes = texBytes;

                auto itCopy = sCopyFboCache.find(handle);
                if (itCopy != sCopyFboCache.end()) {
                    rec.copyFbo = itCopy->second;
                    sCopyFboCache.erase(itCopy);
                }
                auto itFbo = sFboCache.find(handle);
                if (itFbo != sFboCache.end()) {
                    rec.fbo = itFbo->second;
                    rec.hasRtFbo = (rec.fbo != 0);
                    sFboCache.erase(itFbo);
                }
                auto itRbo = sStencilRboCache.find(handle);
                if (itRbo != sStencilRboCache.end()) {
                    rec.stencilRbo = itRbo->second;
                    sStencilRboCache.erase(itRbo);
                }
                sGlesTextureCache.erase(itTex);
                sRenderTargetHandles.erase(handle);
                sGpuCopiedHandles.erase(handle);
                pushToRecyclePool(std::move(rec));
                return;
            }
            if (itTex->second.texId != 0) glDeleteTextures(1, &itTex->second.texId);
            sGlesTextureCache.erase(itTex);
        }
        releaseHandleFromRenderThreadCaches(handle);
    };

    auto allocateTexFromPoolOrDriver = [&](const void* handle, uint32_t w, uint32_t h,
                                           const uint8_t* pixels, size_t pixelBytes,
                                           bool isCompressedBc7, bool allowPooledDirty,
                                           bool needRtFbo, bool* outUsedCompressed) -> GLuint {
        if (outUsedCompressed) *outUsedCompressed = false;
        RecycledGlesResource rec{};
        bool canReuse = (!isCompressedBc7) && (pixels != nullptr || allowPooledDirty);
        if (canReuse && popFromRecyclePool(w, h, needRtFbo, rec)) {
            GLuint texId = rec.texId;
            if (rec.copyFbo != 0) {
                sCopyFboCache[handle] = rec.copyFbo;
            }
            if (needRtFbo && rec.fbo != 0) {
                sFboCache[handle] = rec.fbo;
                if (rec.stencilRbo != 0) {
                    sStencilRboCache[handle] = rec.stencilRbo;
                }
            } else {
                if (rec.fbo != 0) glDeleteFramebuffers(1, &rec.fbo);
                if (rec.stencilRbo != 0) glDeleteRenderbuffers(1, &rec.stencilRbo);
            }
            if (pixels != nullptr) {
                glBindTexture(GL_TEXTURE_2D, texId);
                uploadBoundTexture2D(handle, w, h, pixels, pixelBytes, false, true);
            }
            return texId;
        }

        GLuint newTex = 0;
        glGenTextures(1, &newTex);
        glBindTexture(GL_TEXTURE_2D, newTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const bool usedComp = uploadBoundTexture2D(handle, w, h, pixels, pixelBytes, isCompressedBc7, false);
        if (outUsedCompressed) *outUsedCompressed = usedComp;
        return newTex;
    };

    auto waitForInFlightPreupload = [&](const void* handle) {
        {
            std::unique_lock<std::mutex> lock(readyPreuploadMutex);
            if (inFlightPreuploads.count(handle) != 0) {
                preuploadDoneCv.wait_for(lock, std::chrono::milliseconds(150), [&]() {
                    return inFlightPreuploads.count(handle) == 0 || !preuploadRunning.load();
                });
            }
        }
        drainReadyPreuploads();
    };

    auto getOrCreateGlesTexture = [&](const void* handle, uint32_t w, uint32_t h,
                                      const uint8_t* pixels, uint32_t revision,
                                      uint64_t generationId = 0,
                                      bool allowPooledDirty = false,
                                      bool preferRtFbo = false,
                                      size_t pixelBytes = 0,
                                      bool isCompressedBc7 = false) -> GLuint {
        if (handle == nullptr || w == 0 || h == 0) return 0;
        auto it = sGlesTextureCache.find(handle);
        if (it == sGlesTextureCache.end()) {
            waitForInFlightPreupload(handle);
            it = sGlesTextureCache.find(handle);
        }
        if (it == sGlesTextureCache.end()) {
            auto tUpStart = std::chrono::steady_clock::now();
            bool usedComp = false;
            GLuint newTex = allocateTexFromPoolOrDriver(
                handle, w, h, pixels, pixelBytes, isCompressedBc7, allowPooledDirty, preferRtFbo, &usedComp
            );
            float upMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tUpStart).count();
            if (upMs >= kPerfTraceThresholdMs && pixels != nullptr) {
                LOGI("[PerfTrace] RenderThread texture upload %ux%u (bptc=%d, bytes=%zu) mat %.2fms",
                     w, h, usedComp ? 1 : 0, pixelBytes, upMs);
            }
            sGlesTextureCache[handle] = {newTex, w, h, revision, generationId, usedComp};
            if (pixels != nullptr) {
                D3D11Bridge_MarkTextureUploaded(handle);
            }
            return newTex;
        }

        // Nếu generationId khác biệt: Handle con trỏ này vừa được tái cấp phát cho một Texture mới!
        if (generationId > 0 && it->second.generationId != 0 && it->second.generationId != generationId) {
            if (it->second.texId != 0) {
                glDeleteTextures(1, &it->second.texId);
            }
            releaseHandleFromRenderThreadCaches(handle);
            sGlesTextureCache.erase(it);

            waitForInFlightPreupload(handle);
            auto itFresh = sGlesTextureCache.find(handle);
            if (itFresh != sGlesTextureCache.end() && itFresh->second.generationId == generationId) {
                return itFresh->second.texId;
            }

            bool usedComp = false;
            GLuint newTex = allocateTexFromPoolOrDriver(
                handle, w, h, pixels, pixelBytes, isCompressedBc7, allowPooledDirty, preferRtFbo, &usedComp
            );
            sGlesTextureCache[handle] = {newTex, w, h, revision, generationId, usedComp};
            if (pixels != nullptr) {
                D3D11Bridge_MarkTextureUploaded(handle);
            }
            return newTex;
        }

        // Bảo vệ texture GPU-owned (RenderTarget hoặc đích của GPU CopyResource), chỉ upload lại khi CPU tăng revision
        bool inRtCache = (sFboCache.find(handle) != sFboCache.end() || sRenderTargetHandles.find(handle) != sRenderTargetHandles.end());
        bool isGpuOwned = (inRtCache && D3D11Bridge_IsRenderTargetHandle(handle));
        if (sGpuCopiedHandles.find(handle) != sGpuCopiedHandles.end() || D3D11Bridge_HasGpuContents(handle)) {
            if (pixels != nullptr && it->second.revision != revision && !D3D11Bridge_HasGpuContents(handle)) {
                sGpuCopiedHandles.erase(handle);
                isGpuOwned = false;
            } else {
                isGpuOwned = true;
            }
        }
        if (isGpuOwned && (pixels == nullptr || it->second.revision == revision)) {
            return it->second.texId;
        }

        if (it->second.width != w || it->second.height != h) {
            it->second.width = w;
            it->second.height = h;
            it->second.revision = revision;
            glBindTexture(GL_TEXTURE_2D, it->second.texId);
            it->second.isCompressedBc7 = uploadBoundTexture2D(handle, w, h, pixels, pixelBytes, isCompressedBc7, false);
        } else if (pixels != nullptr && it->second.revision != revision) {
            it->second.revision = revision;
            glBindTexture(GL_TEXTURE_2D, it->second.texId);
            const bool canSubImage = !it->second.isCompressedBc7 && !isCompressedBc7;
            it->second.isCompressedBc7 = uploadBoundTexture2D(handle, w, h, pixels, pixelBytes, isCompressedBc7, canSubImage);
            D3D11Bridge_MarkTextureUploaded(handle);
        }
        return it->second.texId;
    };

    auto getOrCreateFbo = [&](const void* rtHandle, uint32_t w, uint32_t h) -> GLuint {
        auto it = sFboCache.find(rtHandle);
        if (it != sFboCache.end()) return it->second;
        if (D3D11Bridge_IsRenderTargetHandle(rtHandle)) {
            sRenderTargetHandles.insert(rtHandle);
        }

        TextureCpuSnapshot snap{};
        D3D11Bridge_AcquireTextureSnapshot(rtHandle, snap);
        uint32_t effW = snap.width > 0 ? snap.width : w;
        uint32_t effH = snap.height > 0 ? snap.height : h;
        const uint8_t* sTexPixels = snap.pixels ? snap.pixels->data() : nullptr;

        glActiveTexture(GL_TEXTURE0);
        GLuint rtTexId = getOrCreateGlesTexture(rtHandle, effW, effH, sTexPixels, snap.revision, snap.generationId, true, true);

        // Nếu popFromRecyclePool đã khôi phục sẵn rtFbo hoàn chỉnh cho handle này thì trả về ngay
        auto itRecycledFbo = sFboCache.find(rtHandle);
        if (itRecycledFbo != sFboCache.end() && itRecycledFbo->second != 0) {
            glBindTexture(GL_TEXTURE_2D, 0);
            return itRecycledFbo->second;
        }

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glBindTexture(GL_TEXTURE_2D, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rtTexId, 0);

        GLuint stencilRbo = 0;
        glGenRenderbuffers(1, &stencilRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, stencilRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, effW, effH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencilRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            glDeleteRenderbuffers(1, &stencilRbo);
        } else {
            sStencilRboCache[rtHandle] = stencilRbo;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        sFboCache[rtHandle] = fbo;
        return fbo;
    };

    // FBO nhẹ chỉ gắn color attachment (dùng cho copy texture GPU-to-GPU hoặc readback Staging)
    auto getOrCreateCopyFbo = [&](const void* texHandle, uint32_t w, uint32_t h) -> GLuint {
        if (texHandle == nullptr) return 0;
        auto itRt = sFboCache.find(texHandle);
        if (itRt != sFboCache.end() && itRt->second != 0) return itRt->second;
        auto itCopy = sCopyFboCache.find(texHandle);
        if (itCopy != sCopyFboCache.end() && itCopy->second != 0) return itCopy->second;

        TextureCpuSnapshot snap{};
        D3D11Bridge_AcquireTextureSnapshot(texHandle, snap);
        uint32_t effW = snap.width > 0 ? snap.width : w;
        uint32_t effH = snap.height > 0 ? snap.height : h;

        glActiveTexture(GL_TEXTURE0);
        GLuint texId = getOrCreateGlesTexture(texHandle, effW, effH, nullptr, snap.revision, snap.generationId, true, false);
        if (texId == 0) return 0;

        auto itRecycledCopy = sCopyFboCache.find(texHandle);
        if (itRecycledCopy != sCopyFboCache.end() && itRecycledCopy->second != 0) {
            return itRecycledCopy->second;
        }

        GLuint copyFbo = 0;
        glGenFramebuffers(1, &copyFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, copyFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &copyFbo);
            return 0;
        }
        sCopyFboCache[texHandle] = copyFbo;
        return copyFbo;
    };

    // Helper dựng quad copy GPU giữ đúng hướng Top-Down của Texture D3D11
    auto blitTextureToFbo = [&](GLuint srcTexId, GLuint dstFbo,
                                GLint dstX, GLint dstY, GLsizei dstCopyW, GLsizei dstCopyH,
                                float u0, float vTop, float u1, float vBot) {
        if (srcTexId == 0 || dstFbo == 0 || dstCopyW <= 0 || dstCopyH <= 0) return;

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(dstX, dstY, dstCopyW, dstCopyH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTexId);
        glUniform1i(uHasAlphaTextureLoc, 0);
        glUniform1i(uHasDestTextureLoc, 0);

        float identityMat[64] = {0};
        identityMat[0] = 1.0f;
        identityMat[5] = 1.0f;
        identityMat[10] = 1.0f;
        identityMat[15] = 1.0f;
        identityMat[16] = 1.0f;
        identityMat[17] = 1.0f;
        identityMat[18] = 1.0f;
        identityMat[19] = 1.0f;

        glUniform1i(uVsShaderIdLoc, 4);
        glUniform4fv(uVsCb0Loc, 16, identityMat);
        // Trong FBO quy ước D3D11 Top-Down (uFlipY = -1.0f), posY = +1 ánh xạ tới row 0 của FBO -> vTop ứng với mép trên ảnh
        glUniform1f(uFlipYLoc, -1.0f);
        glUniform4f(uD3dViewportLoc, 0.0f, 0.0f, static_cast<float>(dstCopyW), static_cast<float>(dstCopyH));
        glUniform4f(uGlViewportLoc, static_cast<float>(dstX), static_cast<float>(dstY),
                    static_cast<float>(dstCopyW), static_cast<float>(dstCopyH));
        glUniform1i(uShaderIdLoc, 180);

        const float blitVertices[] = {
            -1.0f,  1.0f, 0.0f,  u0, vTop,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
            -1.0f, -1.0f, 0.0f,  u0, vBot,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 0.0f,  u1, vTop,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
             1.0f, -1.0f, 0.0f,  u1, vBot,  0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,
        };

        if (aPositionLoc >= 0) {
            glEnableVertexAttribArray(aPositionLoc);
            glVertexAttribPointer(aPositionLoc, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &blitVertices[0]);
        }
        if (aTexCoord0Loc >= 0) {
            glEnableVertexAttribArray(aTexCoord0Loc);
            glVertexAttribPointer(aTexCoord0Loc, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &blitVertices[3]);
        }
        if (aColorLoc >= 0) {
            glEnableVertexAttribArray(aColorLoc);
            glVertexAttribPointer(aColorLoc, 4, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &blitVertices[7]);
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (aPositionLoc >= 0) glDisableVertexAttribArray(aPositionLoc);
        if (aTexCoord0Loc >= 0) glDisableVertexAttribArray(aTexCoord0Loc);
        if (aColorLoc >= 0) glDisableVertexAttribArray(aColorLoc);
    };

    auto executeMeshCommandBatch = [&](const std::vector<MeshDrawCommand>& batchMeshes,
                                       EGLint effSurfW, EGLint effSurfH,
                                       GLint actX, GLint actY, GLsizei actW, GLsizei actH,
                                       float logicalW, float logicalH) -> bool {
        if (batchMeshes.empty()) return false;

        struct BatchMeshOffsets {
            size_t vertexByteOffset = 0;
            size_t indexByteOffset = 0;
        };
        static std::vector<float> sBatchVertices;
        static std::vector<uint16_t> sBatchIndices;
        static std::vector<BatchMeshOffsets> sBatchOffsets;
        sBatchVertices.clear();
        sBatchIndices.clear();
        sBatchOffsets.resize(batchMeshes.size());

        size_t totalVtxFloats = 0;
        size_t totalIdxCount = 0;
        for (const auto& cmd : batchMeshes) {
            if (cmd.isClearCommand == 0 && cmd.shaderId != 1302 && !cmd.indexData.empty() && !cmd.vertexData.empty()) {
                totalVtxFloats += cmd.vertexData.size();
                totalIdxCount += cmd.indexData.size();
            }
        }
        sBatchVertices.reserve(totalVtxFloats);
        sBatchIndices.reserve(totalIdxCount);
        for (size_t m = 0; m < batchMeshes.size(); ++m) {
            const auto& cmd = batchMeshes[m];
            if (cmd.isClearCommand == 0 && cmd.shaderId != 1302 && !cmd.indexData.empty() && !cmd.vertexData.empty()) {
                sBatchOffsets[m].vertexByteOffset = sBatchVertices.size() * sizeof(float);
                sBatchOffsets[m].indexByteOffset = sBatchIndices.size() * sizeof(uint16_t);
                sBatchVertices.insert(sBatchVertices.end(), cmd.vertexData.begin(), cmd.vertexData.end());
                sBatchIndices.insert(sBatchIndices.end(), cmd.indexData.begin(), cmd.indexData.end());
            }
        }

        if (!sBatchVertices.empty() && !sBatchIndices.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, streamVbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sBatchVertices.size() * sizeof(float)),
                         sBatchVertices.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, streamIbo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sBatchIndices.size() * sizeof(uint16_t)),
                         sBatchIndices.data(), GL_STREAM_DRAW);
        }

        const sgre::GlProgramBinding* curBinding = &uberBinding;
        glUseProgram(curBinding->program);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        const void* lastBoundHandle = nullptr;
        const void* lastAlphaBoundHandle = nullptr;
        const void* lastDestBoundHandle = nullptr;
        const void* currentFboHandle = reinterpret_cast<const void*>(0x1);
        std::unordered_set<const void*> clearedRtsInFrame;
        bool drewBackbuffer = false;

        bool buffersBound = false;
        int cachedVsId = -1;
        int cachedPsId = -1;
        int cachedHasAlpha = -1;
        int cachedHasDest = -1;
        float cachedFlipY = 0.0f;
        float cachedD3dVp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float cachedGlVp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool hasCachedVp = false;
        uint8_t cachedColorMask = 0xFF;
        int cachedStencilEnable = -1;
        int cachedBlendEnable = 1;
        int cachedScissorEnable = -1;

        auto resetStateCache = [&]() {
            curBinding = &uberBinding;
            buffersBound = false;
            cachedVsId = -1;
            cachedPsId = -1;
            cachedHasAlpha = -1;
            cachedHasDest = -1;
            cachedFlipY = 0.0f;
            hasCachedVp = false;
            cachedColorMask = 0xFF;
            cachedStencilEnable = -1;
            cachedBlendEnable = -1;
            cachedScissorEnable = -1;
        };

        for (size_t m = 0; m < batchMeshes.size(); ++m) {
            const auto& cmd = batchMeshes[m];
            if (cmd.isClearCommand != 3 && cmd.renderTargetHandle != nullptr &&
                D3D11Bridge_IsRenderTargetHandle(cmd.renderTargetHandle)) {
                sRenderTargetHandles.insert(cmd.renderTargetHandle);
            }

            if (cmd.isClearCommand == 1 || cmd.isClearCommand == 2) {
                if (cmd.renderTargetHandle != nullptr) {
                    if (lastBoundHandle == cmd.renderTargetHandle) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, whitePixelTexture);
                        lastBoundHandle = reinterpret_cast<const void*>(1);
                    }
                    if (lastAlphaBoundHandle == cmd.renderTargetHandle) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        lastAlphaBoundHandle = nullptr;
                    }
                    if (lastDestBoundHandle == cmd.renderTargetHandle) {
                        glActiveTexture(GL_TEXTURE2);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        lastDestBoundHandle = nullptr;
                    }
                    GLuint fbo = getOrCreateFbo(cmd.renderTargetHandle, cmd.renderTargetWidth, cmd.renderTargetHeight);
                    lastBoundHandle = nullptr;
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glViewport(0, 0, cmd.renderTargetWidth, cmd.renderTargetHeight);
                    currentFboHandle = cmd.renderTargetHandle;
                    if (cachedScissorEnable != 0) {
                        glDisable(GL_SCISSOR_TEST);
                        cachedScissorEnable = 0;
                    }
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(actX, actY, actW, actH);
                    currentFboHandle = nullptr;
                    if (actX != 0 || actY != 0 || actW != effSurfW || actH != effSurfH) {
                        glEnable(GL_SCISSOR_TEST);
                        glScissor(actX, actY, actW, actH);
                        cachedScissorEnable = 1;
                    } else if (cachedScissorEnable != 0) {
                        glDisable(GL_SCISSOR_TEST);
                        cachedScissorEnable = 0;
                    }
                }
                if (cmd.isClearCommand == 1) {
                    clearedRtsInFrame.insert(cmd.renderTargetHandle);
                    if (cachedColorMask != 0x0F) {
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        cachedColorMask = 0x0F;
                    }
                    glClearColor(cmd.clearColor[0], cmd.clearColor[1], cmd.clearColor[2], cmd.clearColor[3]);
                    glClear(GL_COLOR_BUFFER_BIT);
                } else if (cmd.isClearCommand == 2) {
                    glStencilMask(0xFF);
                    glClearStencil(cmd.clearStencil);
                    glClear(GL_STENCIL_BUFFER_BIT);
                }
                continue;
            }

            if (cmd.isClearCommand == 3) {
                // GPU-to-GPU Subresource Copy / CopyResource thuần VRAM (MCapture::draw, MTexture::bind, Capture::storeThumbnail)
                if (lastBoundHandle == cmd.textureHandle) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, whitePixelTexture);
                    lastBoundHandle = reinterpret_cast<const void*>(1);
                }
                if (lastAlphaBoundHandle == cmd.textureHandle) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    lastAlphaBoundHandle = nullptr;
                }
                if (lastDestBoundHandle == cmd.textureHandle) {
                    glActiveTexture(GL_TEXTURE2);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    lastDestBoundHandle = nullptr;
                }

                TextureCpuSnapshot dstSnap{};
                D3D11Bridge_AcquireTextureSnapshot(cmd.textureHandle, dstSnap);
                uint32_t dW = dstSnap.width > 0 ? dstSnap.width : (cmd.textureWidth > 0 ? cmd.textureWidth : 1920);
                uint32_t dH = dstSnap.height > 0 ? dstSnap.height : (cmd.textureHeight > 0 ? cmd.textureHeight : 1080);
                const uint8_t* dstPixels = dstSnap.pixels ? dstSnap.pixels->data() : nullptr;

                GLint dstX = static_cast<GLint>(cmd.viewportX);
                GLint dstY = static_cast<GLint>(cmd.viewportY);
                GLint srcX = cmd.scissorX;
                GLint srcY = cmd.scissorY;
                GLsizei maxDstW = std::max<GLsizei>(0, static_cast<GLsizei>(dW) - dstX);
                GLsizei maxDstH = std::max<GLsizei>(0, static_cast<GLsizei>(dH) - dstY);
                GLsizei reqW = cmd.scissorW > 0 ? cmd.scissorW : static_cast<GLsizei>(dW);
                GLsizei reqH = cmd.scissorH > 0 ? cmd.scissorH : static_cast<GLsizei>(dH);
                GLsizei copyW = std::min(reqW, maxDstW);
                GLsizei copyH = std::min(reqH, maxDstH);
                bool isFullDstCopy = (dstX == 0 && dstY == 0 && copyW == static_cast<GLsizei>(dW) && copyH == static_cast<GLsizei>(dH));

                glActiveTexture(GL_TEXTURE0);
                GLuint dstTex = getOrCreateGlesTexture(
                    cmd.textureHandle, dW, dH,
                    isFullDstCopy ? nullptr : dstPixels,
                    dstSnap.revision, dstSnap.generationId,
                    isFullDstCopy, false
                );
                lastBoundHandle = nullptr;

                if (dstTex != 0 && copyW > 0 && copyH > 0) {
                    if (cmd.renderTargetHandle == nullptr) {
                        // Nguồn là vùng khung hình hoạt động 16:9 trên Backbuffer (Bottom-Up):
                        bool didHardwareBlit = false;
                        if (isGles3Context && sSupportsBackbufferBlit && reqW == copyW && reqH == copyH) {
                            GLuint dstFbo = getOrCreateCopyFbo(cmd.textureHandle, dW, dH);
                            if (dstFbo != 0) {
                                float srcLogicalW = logicalW > 0.0f ? logicalW : 1920.0f;
                                float srcLogicalH = logicalH > 0.0f ? logicalH : 1080.0f;
                                float scaleX = static_cast<float>(actW) / srcLogicalW;
                                float scaleY = static_cast<float>(actH) / srcLogicalH;
                                GLint sx0 = actX + std::max<GLint>(0, std::min<GLint>(actW, static_cast<GLint>(std::lround(srcX * scaleX))));
                                GLint sx1 = actX + std::max<GLint>(0, std::min<GLint>(actW, static_cast<GLint>(std::lround((srcX + reqW) * scaleX))));
                                GLint syTop = actY + std::max<GLint>(0, std::min<GLint>(actH, static_cast<GLint>(std::lround(static_cast<float>(actH) - srcY * scaleY))));
                                GLint syBot = actY + std::max<GLint>(0, std::min<GLint>(actH, static_cast<GLint>(std::lround(static_cast<float>(actH) - (srcY + reqH) * scaleY))));

                                if (!sTestedBackbufferBlit) {
                                    while (glGetError() != GL_NO_ERROR) {}
                                }
                                glDisable(GL_SCISSOR_TEST);
                                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
                                glBlitFramebuffer(sx0, syTop, sx1, syBot, dstX, dstY, dstX + copyW, dstY + copyH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                                if (!sTestedBackbufferBlit) {
                                    GLenum blitErr = glGetError();
                                    sTestedBackbufferBlit = true;
                                    if (blitErr != GL_NO_ERROR) {
                                        LOGW("glBlitFramebuffer tu Backbuffer khong duoc ho tro (err=0x%X), fallback sang blitTextureToFbo", blitErr);
                                        sSupportsBackbufferBlit = false;
                                    } else {
                                        LOGI("glBlitFramebuffer 1-pass Backbuffer Copy da duoc kich hoat thanh cong");
                                        didHardwareBlit = true;
                                    }
                                } else {
                                    didHardwareBlit = true;
                                }
                            }
                        }

                        if (!didHardwareBlit) {
                            if (sBackbufferFlipTempTex == 0 ||
                                sBackbufferFlipTempW != static_cast<uint32_t>(actW) ||
                                sBackbufferFlipTempH != static_cast<uint32_t>(actH)) {
                                if (sBackbufferFlipTempTex == 0) {
                                    glGenTextures(1, &sBackbufferFlipTempTex);
                                }
                                glBindTexture(GL_TEXTURE_2D, sBackbufferFlipTempTex);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, actW, actH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                                sBackbufferFlipTempW = static_cast<uint32_t>(actW);
                                sBackbufferFlipTempH = static_cast<uint32_t>(actH);
                            }
                            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                            glBindTexture(GL_TEXTURE_2D, sBackbufferFlipTempTex);
                            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, actX, actY, actW, actH);

                            GLuint dstFbo = getOrCreateCopyFbo(cmd.textureHandle, dW, dH);
                            float srcLogicalW = logicalW > 0.0f ? logicalW : 1920.0f;
                            float srcLogicalH = logicalH > 0.0f ? logicalH : 1080.0f;
                            float u0 = static_cast<float>(srcX) / srcLogicalW;
                            float u1 = static_cast<float>(srcX + reqW) / srcLogicalW;
                            float vTop = 1.0f - (static_cast<float>(srcY) / srcLogicalH);
                            float vBot = 1.0f - (static_cast<float>(srcY + reqH) / srcLogicalH);
                            blitTextureToFbo(sBackbufferFlipTempTex, dstFbo, dstX, dstY, copyW, copyH, u0, vTop, u1, vBot);
                            resetStateCache();
                        } else {
                            cachedScissorEnable = 0;
                        }
                        currentFboHandle = cmd.textureHandle;
                    } else {
                        // Nguồn là Offscreen FBO hoặc Staging GPU-owned (luôn Top-Down sẵn):
                        TextureCpuSnapshot srcSnap{};
                        D3D11Bridge_AcquireTextureSnapshot(cmd.renderTargetHandle, srcSnap);
                        uint32_t sW = srcSnap.width > 0 ? srcSnap.width : (cmd.renderTargetWidth > 0 ? cmd.renderTargetWidth : 1920);
                        uint32_t sH = srcSnap.height > 0 ? srcSnap.height : (cmd.renderTargetHeight > 0 ? cmd.renderTargetHeight : 1080);

                        if (reqW == copyW && reqH == copyH) {
                            GLuint srcFbo = getOrCreateCopyFbo(cmd.renderTargetHandle, sW, sH);
                            if (srcFbo != 0) {
                                glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
                                glActiveTexture(GL_TEXTURE0);
                                glBindTexture(GL_TEXTURE_2D, dstTex);
                                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dstX, dstY, srcX, srcY, copyW, copyH);
                            }
                        } else {
                            GLuint srcTex = getOrCreateGlesTexture(cmd.renderTargetHandle, sW, sH, nullptr, srcSnap.revision, srcSnap.generationId, true, false);
                            GLuint dstFbo = getOrCreateCopyFbo(cmd.textureHandle, dW, dH);
                            float u0 = static_cast<float>(srcX) / static_cast<float>(sW);
                            float u1 = static_cast<float>(srcX + reqW) / static_cast<float>(sW);
                            float vTop = static_cast<float>(srcY) / static_cast<float>(sH);
                            float vBot = static_cast<float>(srcY + reqH) / static_cast<float>(sH);
                            blitTextureToFbo(srcTex, dstFbo, dstX, dstY, copyW, copyH, u0, vTop, u1, vBot);
                            resetStateCache();
                            currentFboHandle = cmd.textureHandle;
                        }
                    }

                    sGpuCopiedHandles.insert(cmd.textureHandle);
                    auto itDst = sGlesTextureCache.find(cmd.textureHandle);
                    if (itDst != sGlesTextureCache.end()) {
                        itDst->second.revision = dstSnap.revision;
                        if (dstSnap.generationId > 0) itDst->second.generationId = dstSnap.generationId;
                    }
                }

                glBindTexture(GL_TEXTURE_2D, 0);
                lastBoundHandle = nullptr;
                lastAlphaBoundHandle = nullptr;
                lastDestBoundHandle = nullptr;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                currentFboHandle = reinterpret_cast<const void*>(0x1);
                continue;
            }

            if (cmd.indexData.empty() || cmd.vertexData.empty()) continue;
            if (cmd.shaderId == 1302) continue;

            // Chọn chương trình GLSL chuyên biệt theo họ shader (tránh thực thi nhánh blur nặng trên sprite thường)
            const sgre::GlProgramBinding& desiredBinding = programSet.selectForShaderId(cmd.shaderId);
            if (desiredBinding.program != curBinding->program) {
                if (buffersBound) {
                    if (curBinding->aPositionLoc >= 0) glDisableVertexAttribArray(curBinding->aPositionLoc);
                    if (curBinding->aTexCoord0Loc >= 0) glDisableVertexAttribArray(curBinding->aTexCoord0Loc);
                    if (curBinding->aTexCoord1Loc >= 0) glDisableVertexAttribArray(curBinding->aTexCoord1Loc);
                    if (curBinding->aColorLoc >= 0) glDisableVertexAttribArray(curBinding->aColorLoc);
                    buffersBound = false;
                }
                curBinding = &desiredBinding;
                glUseProgram(curBinding->program);
                cachedVsId = -1;
                cachedPsId = -1;
                cachedHasAlpha = -1;
                cachedHasDest = -1;
                cachedFlipY = 0.0f;
                if (hasCachedVp) {
                    if (curBinding->uD3dViewportLoc >= 0) {
                        glUniform4f(curBinding->uD3dViewportLoc, cachedD3dVp[0], cachedD3dVp[1], cachedD3dVp[2], cachedD3dVp[3]);
                    }
                    if (curBinding->uGlViewportLoc >= 0) {
                        glUniform4f(curBinding->uGlViewportLoc, cachedGlVp[0], cachedGlVp[1], cachedGlVp[2], cachedGlVp[3]);
                    }
                }
            }

            const void* targetHandle = cmd.renderTargetHandle;
            if (targetHandle != nullptr) {
                if (lastBoundHandle == targetHandle) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, whitePixelTexture);
                    lastBoundHandle = reinterpret_cast<const void*>(1);
                }
                if (lastAlphaBoundHandle == targetHandle) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    lastAlphaBoundHandle = nullptr;
                }
                if (lastDestBoundHandle == targetHandle) {
                    glActiveTexture(GL_TEXTURE2);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    lastDestBoundHandle = nullptr;
                }
                if (targetHandle != currentFboHandle) {
                    GLuint fbo = getOrCreateFbo(targetHandle, cmd.renderTargetWidth, cmd.renderTargetHeight);
                    lastBoundHandle = nullptr;
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    currentFboHandle = targetHandle;
                }
                clearedRtsInFrame.erase(targetHandle);
                float rtW = cmd.renderTargetWidth > 0 ? static_cast<float>(cmd.renderTargetWidth) : logicalW;
                float rtH = cmd.renderTargetHeight > 0 ? static_cast<float>(cmd.renderTargetHeight) : logicalH;
                float vpX = cmd.viewportX;
                float vpY = cmd.viewportY;
                float vpW = cmd.viewportW > 0.0f ? cmd.viewportW : rtW;
                float vpH = cmd.viewportH > 0.0f ? cmd.viewportH : rtH;
                glViewport(
                    static_cast<GLint>(vpX + 0.5f),
                    static_cast<GLint>(vpY + 0.5f),
                    std::max<GLsizei>(1, static_cast<GLsizei>(vpW + 0.5f)),
                    std::max<GLsizei>(1, static_cast<GLsizei>(vpH + 0.5f))
                );
                if (cachedFlipY != -1.0f) {
                    if (curBinding->uFlipYLoc >= 0) glUniform1f(curBinding->uFlipYLoc, -1.0f);
                    cachedFlipY = -1.0f;
                }
                cachedD3dVp[0] = vpX; cachedD3dVp[1] = vpY; cachedD3dVp[2] = vpW; cachedD3dVp[3] = vpH;
                cachedGlVp[0] = vpX; cachedGlVp[1] = vpY; cachedGlVp[2] = std::max(1.0f, vpW); cachedGlVp[3] = std::max(1.0f, vpH);
                hasCachedVp = true;
                if (curBinding->uD3dViewportLoc >= 0) glUniform4f(curBinding->uD3dViewportLoc, vpX, vpY, vpW, vpH);
                if (curBinding->uGlViewportLoc >= 0) glUniform4f(curBinding->uGlViewportLoc, vpX, vpY, std::max(1.0f, vpW), std::max(1.0f, vpH));
            } else {
                if (currentFboHandle != nullptr) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    currentFboHandle = nullptr;
                }
                float vpX = cmd.viewportX;
                float vpY = cmd.viewportY;
                float vpW = cmd.viewportW > 0.0f ? cmd.viewportW : logicalW;
                float vpH = cmd.viewportH > 0.0f ? cmd.viewportH : logicalH;
                float scaleX = static_cast<float>(actW) / logicalW;
                float scaleY = static_cast<float>(actH) / logicalH;
                float glVpX = static_cast<float>(actX) + vpX * scaleX;
                float glVpW = vpW * scaleX;
                float glVpH = vpH * scaleY;
                float glVpY = static_cast<float>(actY + actH) - (vpY + vpH) * scaleY;
                glViewport(
                    static_cast<GLint>(glVpX + 0.5f),
                    static_cast<GLint>(glVpY + 0.5f),
                    std::max<GLsizei>(1, static_cast<GLsizei>(glVpW + 0.5f)),
                    std::max<GLsizei>(1, static_cast<GLsizei>(glVpH + 0.5f))
                );
                if (cachedFlipY != 1.0f) {
                    if (curBinding->uFlipYLoc >= 0) glUniform1f(curBinding->uFlipYLoc, 1.0f);
                    cachedFlipY = 1.0f;
                }
                cachedD3dVp[0] = vpX; cachedD3dVp[1] = vpY; cachedD3dVp[2] = vpW; cachedD3dVp[3] = vpH;
                cachedGlVp[0] = glVpX; cachedGlVp[1] = glVpY; cachedGlVp[2] = std::max(1.0f, glVpW); cachedGlVp[3] = std::max(1.0f, glVpH);
                hasCachedVp = true;
                if (curBinding->uD3dViewportLoc >= 0) glUniform4f(curBinding->uD3dViewportLoc, vpX, vpY, vpW, vpH);
                if (curBinding->uGlViewportLoc >= 0) glUniform4f(curBinding->uGlViewportLoc, glVpX, glVpY, std::max(1.0f, glVpW), std::max(1.0f, glVpH));
            }

            glActiveTexture(GL_TEXTURE0);
            if (cmd.textureHandle != nullptr && cmd.textureHandle != targetHandle) {
                if (cmd.textureHandle != lastBoundHandle) {
                    TextureCpuSnapshot snap{};
                    if (D3D11Bridge_AcquireTextureSnapshot(cmd.textureHandle, snap)) {
                        const uint8_t* sTexPixels = snap.pixels ? snap.pixels->data() : nullptr;
                        const size_t sTexBytes = snap.pixels ? snap.pixels->size() : 0;
                        GLuint meshTexId = getOrCreateGlesTexture(
                            cmd.textureHandle, snap.width, snap.height, sTexPixels, snap.revision,
                            snap.generationId, false, false, sTexBytes, snap.isCompressedBc7
                        );
                        if (meshTexId != 0) {
                            glBindTexture(GL_TEXTURE_2D, meshTexId);
                            lastBoundHandle = cmd.textureHandle;
                        }
                    }
                }
            } else {
                if (lastBoundHandle != reinterpret_cast<const void*>(1)) {
                    glBindTexture(GL_TEXTURE_2D, whitePixelTexture);
                    lastBoundHandle = reinterpret_cast<const void*>(1);
                }
            }

            bool hasValidAlphaMask = false;
            if (cmd.alphaTextureHandle != nullptr && cmd.alphaTextureHandle != targetHandle) {
                TextureCpuSnapshot alphaSnap{};
                D3D11Bridge_AcquireTextureSnapshot(cmd.alphaTextureHandle, alphaSnap);
                const uint8_t* aTexPixels = alphaSnap.pixels ? alphaSnap.pixels->data() : nullptr;
                const size_t aTexBytes = alphaSnap.pixels ? alphaSnap.pixels->size() : 0;
                glActiveTexture(GL_TEXTURE1);
                GLuint alphaTexId = getOrCreateGlesTexture(
                    cmd.alphaTextureHandle,
                    alphaSnap.width > 0 ? alphaSnap.width : cmd.alphaTextureWidth,
                    alphaSnap.height > 0 ? alphaSnap.height : cmd.alphaTextureHeight,
                    aTexPixels, alphaSnap.revision, alphaSnap.generationId,
                    false, false, aTexBytes, alphaSnap.isCompressedBc7
                );
                if (alphaTexId != 0) {
                    if (cmd.alphaTextureHandle != lastAlphaBoundHandle) {
                        glBindTexture(GL_TEXTURE_2D, alphaTexId);
                        lastAlphaBoundHandle = cmd.alphaTextureHandle;
                    }
                    hasValidAlphaMask = true;
                }
            }
            if (!hasValidAlphaMask) {
                lastAlphaBoundHandle = nullptr;
            }
            const int hasAlphaInt = hasValidAlphaMask ? 1 : 0;
            if (cachedHasAlpha != hasAlphaInt) {
                if (curBinding->uHasAlphaTextureLoc >= 0) glUniform1i(curBinding->uHasAlphaTextureLoc, hasAlphaInt);
                cachedHasAlpha = hasAlphaInt;
            }

            bool hasValidDestTex = false;
            if (curBinding->uHasDestTextureLoc >= 0 && cmd.destTextureHandle != nullptr && cmd.destTextureHandle != targetHandle) {
                TextureCpuSnapshot destSnap{};
                D3D11Bridge_AcquireTextureSnapshot(cmd.destTextureHandle, destSnap);
                const uint8_t* dTexPixels = destSnap.pixels ? destSnap.pixels->data() : nullptr;
                const size_t dTexBytes = destSnap.pixels ? destSnap.pixels->size() : 0;
                glActiveTexture(GL_TEXTURE2);
                GLuint destTexId = getOrCreateGlesTexture(
                    cmd.destTextureHandle,
                    destSnap.width > 0 ? destSnap.width : cmd.destTextureWidth,
                    destSnap.height > 0 ? destSnap.height : cmd.destTextureHeight,
                    dTexPixels,
                    destSnap.revision,
                    destSnap.generationId,
                    false, false, dTexBytes, destSnap.isCompressedBc7
                );
                if (destTexId != 0) {
                    if (cmd.destTextureHandle != lastDestBoundHandle) {
                        glBindTexture(GL_TEXTURE_2D, destTexId);
                        lastDestBoundHandle = cmd.destTextureHandle;
                    }
                    hasValidDestTex = true;
                }
            }
            if (!hasValidDestTex) {
                lastDestBoundHandle = nullptr;
            }
            const int hasDestInt = hasValidDestTex ? 1 : 0;
            if (cachedHasDest != hasDestInt) {
                if (curBinding->uHasDestTextureLoc >= 0) glUniform1i(curBinding->uHasDestTextureLoc, hasDestInt);
                cachedHasDest = hasDestInt;
            }

            const int vsIdInt = static_cast<int>(cmd.vsShaderId);
            if (cachedVsId != vsIdInt) {
                if (curBinding->uVsShaderIdLoc >= 0) glUniform1i(curBinding->uVsShaderIdLoc, vsIdInt);
                cachedVsId = vsIdInt;
            }
            if (curBinding->uVsCb0Loc >= 0) glUniform4fv(curBinding->uVsCb0Loc, 16, cmd.vsCb0);

            const int psIdInt = static_cast<int>(cmd.shaderId);
            if (cachedPsId != psIdInt) {
                if (curBinding->uShaderIdLoc >= 0) glUniform1i(curBinding->uShaderIdLoc, psIdInt);
                cachedPsId = psIdInt;
            }
            if (curBinding->uPsCb0Loc >= 0) glUniform4fv(curBinding->uPsCb0Loc, 8, cmd.cb0);

            if (cmd.stencilEnable) {
                if (cachedStencilEnable != 1) {
                    glEnable(GL_STENCIL_TEST);
                    cachedStencilEnable = 1;
                }
                glStencilFunc(cmd.stencilFunc != 0 ? cmd.stencilFunc : GL_ALWAYS, cmd.stencilRef, cmd.stencilReadMask);
                glStencilMask(cmd.stencilWriteMask);
                glStencilOp(cmd.stencilFailOp, cmd.stencilDepthFailOp, cmd.stencilPassOp);
            } else if (cachedStencilEnable != 0) {
                glDisable(GL_STENCIL_TEST);
                cachedStencilEnable = 0;
            }

            if (cmd.colorWriteMask != cachedColorMask) {
                if (cmd.colorWriteMask == 0) {
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                } else {
                    glColorMask(
                        (cmd.colorWriteMask & 1) ? GL_TRUE : GL_FALSE,
                        (cmd.colorWriteMask & 2) ? GL_TRUE : GL_FALSE,
                        (cmd.colorWriteMask & 4) ? GL_TRUE : GL_FALSE,
                        (cmd.colorWriteMask & 8) ? GL_TRUE : GL_FALSE
                    );
                }
                cachedColorMask = cmd.colorWriteMask;
            }

            if (cmd.blendEnable) {
                if (cachedBlendEnable != 1) {
                    glEnable(GL_BLEND);
                    cachedBlendEnable = 1;
                }
                if (cmd.srcBlend == GL_CONSTANT_COLOR || cmd.destBlend == GL_CONSTANT_COLOR ||
                    cmd.srcBlend == GL_ONE_MINUS_CONSTANT_COLOR || cmd.destBlend == GL_ONE_MINUS_CONSTANT_COLOR) {
                    glBlendColor(cmd.blendFactor[0], cmd.blendFactor[1], cmd.blendFactor[2], cmd.blendFactor[3]);
                }
                if (cmd.shaderId >= 900 && cmd.shaderId < 2040 && !hasValidDestTex) {
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    glBlendFuncSeparate(cmd.srcBlend, cmd.destBlend, cmd.srcBlendAlpha, cmd.destBlendAlpha);
                }
                glBlendEquationSeparate(
                    cmd.blendOp != 0 ? cmd.blendOp : GL_FUNC_ADD,
                    cmd.blendOpAlpha != 0 ? cmd.blendOpAlpha : GL_FUNC_ADD
                );
            } else if (cachedBlendEnable != 0) {
                glDisable(GL_BLEND);
                cachedBlendEnable = 0;
            }

            if (cmd.scissorEnable) {
                if (cachedScissorEnable != 1) {
                    glEnable(GL_SCISSOR_TEST);
                    cachedScissorEnable = 1;
                }
                if (targetHandle != nullptr) {
                    glScissor(cmd.scissorX, cmd.scissorY, std::max(0, cmd.scissorW), std::max(0, cmd.scissorH));
                } else {
                    float scaleX = static_cast<float>(actW) / logicalW;
                    float scaleY = static_cast<float>(actH) / logicalH;
                    GLint scX = actX + static_cast<GLint>(cmd.scissorX * scaleX + 0.5f);
                    GLsizei scW = std::max<GLsizei>(0, static_cast<GLsizei>(cmd.scissorW * scaleX + 0.5f));
                    GLsizei scH = std::max<GLsizei>(0, static_cast<GLsizei>(cmd.scissorH * scaleY + 0.5f));
                    GLint scY = actY + std::max<GLint>(0, actH - static_cast<GLint>((cmd.scissorY + cmd.scissorH) * scaleY + 0.5f));
                    glScissor(scX, scY, scW, scH);
                }
            } else if (targetHandle == nullptr && (actX != 0 || actY != 0 || actW != effSurfW || actH != effSurfH)) {
                // Giữ vùng cắt nằm gọn trong khung 16:9 để tránh tràn đồ họa sang viền đen Pillarbox/Letterbox
                if (cachedScissorEnable != 1) {
                    glEnable(GL_SCISSOR_TEST);
                    cachedScissorEnable = 1;
                }
                glScissor(actX, actY, actW, actH);
            } else if (cachedScissorEnable != 0) {
                glDisable(GL_SCISSOR_TEST);
                cachedScissorEnable = 0;
            }

            if (!buffersBound) {
                glBindBuffer(GL_ARRAY_BUFFER, streamVbo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, streamIbo);
                if (curBinding->aPositionLoc >= 0) glEnableVertexAttribArray(curBinding->aPositionLoc);
                if (curBinding->aTexCoord0Loc >= 0) glEnableVertexAttribArray(curBinding->aTexCoord0Loc);
                if (curBinding->aTexCoord1Loc >= 0) glEnableVertexAttribArray(curBinding->aTexCoord1Loc);
                if (curBinding->aColorLoc >= 0) glEnableVertexAttribArray(curBinding->aColorLoc);
                buffersBound = true;
            }

            const uintptr_t baseByte = sBatchOffsets[m].vertexByteOffset;
            if (curBinding->aPositionLoc >= 0) {
                glVertexAttribPointer(curBinding->aPositionLoc, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<const void*>(baseByte));
            }
            if (curBinding->aTexCoord0Loc >= 0) {
                glVertexAttribPointer(curBinding->aTexCoord0Loc, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<const void*>(baseByte + 3 * sizeof(float)));
            }
            if (curBinding->aTexCoord1Loc >= 0) {
                glVertexAttribPointer(curBinding->aTexCoord1Loc, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<const void*>(baseByte + 5 * sizeof(float)));
            }
            if (curBinding->aColorLoc >= 0) {
                glVertexAttribPointer(curBinding->aColorLoc, 4, GL_FLOAT, GL_FALSE, 11 * sizeof(float), reinterpret_cast<const void*>(baseByte + 7 * sizeof(float)));
            }

            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.indexData.size()), GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(sBatchOffsets[m].indexByteOffset));
            if (currentFboHandle == nullptr) {
                drewBackbuffer = true;
            }
        }

        if (buffersBound) {
            if (curBinding->aPositionLoc >= 0) glDisableVertexAttribArray(curBinding->aPositionLoc);
            if (curBinding->aTexCoord0Loc >= 0) glDisableVertexAttribArray(curBinding->aTexCoord0Loc);
            if (curBinding->aTexCoord1Loc >= 0) glDisableVertexAttribArray(curBinding->aTexCoord1Loc);
            if (curBinding->aColorLoc >= 0) glDisableVertexAttribArray(curBinding->aColorLoc);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }

        if (currentFboHandle != nullptr) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, effSurfW, effSurfH);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        return drewBackbuffer;
    };

    auto processPendingFlushRequests = [&](EGLint effSurfW, EGLint effSurfH,
                                           GLint actX, GLint actY, GLsizei actW, GLsizei actH,
                                           float logicalW, float logicalH) {
        GpuFlushRequest flushReq;
        while (D3D11Bridge_ConsumeFlushRequest(flushReq)) {
            if (!flushReq.commands.empty()) {
                executeMeshCommandBatch(flushReq.commands, effSurfW, effSurfH, actX, actY, actW, actH, logicalW, logicalH);
            }
            bool readOk = false;
            if (flushReq.targetHandle != nullptr && flushReq.dstPtr != nullptr &&
                flushReq.width > 0 && flushReq.height > 0) {
                GLuint readFbo = getOrCreateCopyFbo(flushReq.targetHandle, flushReq.width, flushReq.height);
                if (readFbo != 0) {
                    std::vector<uint8_t> rawBuf(static_cast<size_t>(flushReq.width) * flushReq.height * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
                    glReadPixels(0, 0, flushReq.width, flushReq.height, GL_RGBA, GL_UNSIGNED_BYTE, rawBuf.data());
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    GLenum err = glGetError();
                    if (err == GL_NO_ERROR) {
                        for (uint32_t y = 0; y < flushReq.height; ++y) {
                            const uint8_t* pSrcRow = rawBuf.data() + static_cast<size_t>(y) * flushReq.width * 4;
                            uint8_t* pDstRow = flushReq.dstPtr + static_cast<size_t>(y) * flushReq.rowPitch;
                            if (flushReq.toBgra) {
                                swapRgbaToBgraRow(pSrcRow, pDstRow, flushReq.width);
                            } else {
                                memcpy(pDstRow, pSrcRow, static_cast<size_t>(flushReq.width) * 4);
                            }
                        }
                        readOk = true;
                    } else {
                        LOGE("processPendingFlushRequests: glReadPixels thất bại err=0x%X (fbo=%u)", err, readFbo);
                    }
                }
            }
            D3D11Bridge_CompleteFlushRequest(readOk);
        }
    };

    EGLSurface uploadPbuffer = EGL_NO_SURFACE;
    EGLContext uploadContext = EGL_NO_CONTEXT;
    std::thread uploadThread;

    if (isGles3Context) {
        const EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        uploadPbuffer = eglCreatePbufferSurface(display, config, pbufferAttribs);
        if (uploadPbuffer != EGL_NO_SURFACE) {
            const EGLint shareAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            uploadContext = eglCreateContext(display, config, context, shareAttribs);
        }
    }

    if (uploadContext != EGL_NO_CONTEXT && uploadPbuffer != EGL_NO_SURFACE) {
        LOGI("Shared EGL Context Pre-Uploader da san sang cho asset loading bat dong bo");
        uploadThread = std::thread([&]() {
            if (!eglMakeCurrent(display, uploadPbuffer, uploadPbuffer, uploadContext)) {
                LOGW("Shared EGL Pre-Uploader eglMakeCurrent that bai, dung fallback tren Render Thread");
                return;
            }
            sharedUploaderActive.store(true);
            std::vector<TextureUploadRequest> reqs;
            while (gIsRunning.load() && preuploadRunning.load()) {
                if (!D3D11Bridge_WaitAndConsumePreuploadRequests(reqs, 50)) {
                    continue;
                }
                for (const auto& req : reqs) {
                    if (!gIsRunning.load() || !preuploadRunning.load()) break;
                    TextureCpuSnapshot snap{};
                    if (!D3D11Bridge_AcquireTextureSnapshot(req.handle, snap)) continue;
                    if (snap.generationId != req.generationId || snap.revision != req.revision) continue;
                    if (!snap.pixels || snap.pixels->empty()) continue;

                    size_t expectedBytes = 0;
                    if (snap.isCompressedBc7) {
                        const size_t bx = (snap.width + 3u) / 4u;
                        const size_t by = (snap.height + 3u) / 4u;
                        expectedBytes = bx * by * 16ull;
                    } else {
                        expectedBytes = static_cast<size_t>(snap.width) * snap.height * 4ull;
                    }
                    if (snap.pixels->size() < expectedBytes) continue;

                    {
                        std::lock_guard<std::mutex> lock(readyPreuploadMutex);
                        inFlightPreuploads.insert(req.handle);
                    }

                    auto tUpStart = std::chrono::steady_clock::now();
                    GLuint newTex = 0;
                    glGenTextures(1, &newTex);
                    glBindTexture(GL_TEXTURE_2D, newTex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    const bool usedComp = uploadBoundTexture2D(
                        req.handle, snap.width, snap.height, snap.pixels->data(),
                        snap.pixels->size(), snap.isCompressedBc7, false
                    );
                    glBindTexture(GL_TEXTURE_2D, 0);
                    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();
                    float upMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tUpStart).count();
                    if (upMs >= kPerfTraceThresholdMs) {
                        LOGI("[PerfTrace] SharedEGL pre-upload %ux%u (bptc=%d, %zuKB) mat %.2fms",
                             snap.width, snap.height, usedComp ? 1 : 0, snap.pixels->size() / 1024u, upMs);
                    }

                    ReadyPreuploadTexture item{};
                    item.handle = req.handle;
                    item.texId = newTex;
                    item.width = snap.width;
                    item.height = snap.height;
                    item.revision = snap.revision;
                    item.generationId = snap.generationId;
                    item.isCompressedBc7 = usedComp;
                    item.fence = fence;

                    {
                        std::lock_guard<std::mutex> lock(readyPreuploadMutex);
                        inFlightPreuploads.erase(req.handle);
                        readyPreuploads.push_back(item);
                    }
                    preuploadDoneCv.notify_all();
                }
            }
            sharedUploaderActive.store(false);
            preuploadDoneCv.notify_all();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        });
    } else {
        if (uploadPbuffer != EGL_NO_SURFACE) {
            eglDestroySurface(display, uploadPbuffer);
            uploadPbuffer = EGL_NO_SURFACE;
        }
        LOGI("Shared EGL Context khong kha dung, su dung time-sliced pre-upload fallback tren Render Thread");
    }

    auto processFallbackPreuploads = [&](float budgetMs) {
        if (sharedUploaderActive.load()) return;
        std::vector<TextureUploadRequest> incoming;
        D3D11Bridge_ConsumePreuploadRequests(incoming);
        if (!incoming.empty()) {
            fallbackPreuploadPending.insert(fallbackPreuploadPending.end(), incoming.begin(), incoming.end());
        }
        if (fallbackPreuploadPending.empty()) return;

        auto tStart = std::chrono::steady_clock::now();
        size_t idx = 0;
        for (; idx < fallbackPreuploadPending.size(); ++idx) {
            const auto& req = fallbackPreuploadPending[idx];
            TextureCpuSnapshot snap{};
            if (D3D11Bridge_AcquireTextureSnapshot(req.handle, snap) &&
                snap.generationId == req.generationId &&
                snap.revision == req.revision &&
                snap.pixels && !snap.pixels->empty()) {
                getOrCreateGlesTexture(req.handle, snap.width, snap.height, snap.pixels->data(),
                                       snap.revision, snap.generationId, false, false,
                                       snap.pixels->size(), snap.isCompressedBc7);
            }
            float elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tStart).count();
            if (elapsed >= budgetMs) {
                ++idx;
                break;
            }
        }
        if (idx >= fallbackPreuploadPending.size()) {
            fallbackPreuploadPending.clear();
        } else if (idx > 0) {
            fallbackPreuploadPending.erase(fallbackPreuploadPending.begin(),
                                           fallbackPreuploadPending.begin() + static_cast<std::ptrdiff_t>(idx));
        }
    };

    LOGI("Bắt đầu vòng lặp Render đồ hoạ đồng bộ theo D3D11 SwapChain...");
    D3D11Bridge_SetRenderLoopActive(true);

    RenderFrameState frameState = {};
    bool hasReceivedFirstFrame = false;
    auto fpsWindowStart = std::chrono::steady_clock::now();
    uint32_t fpsFrameCount = 0;

    while (gIsRunning.load()) {
        const int32_t desiredFps = gTargetFps.load(std::memory_order_relaxed);
        if (desiredFps != appliedTargetFps) {
            appliedTargetFps = desiredFps;
            eglSwapInterval(display, appliedTargetFps <= 30 ? 2 : 1);
            LOGI("Đã cập nhật giới hạn nhịp khung hình EGL: %d FPS (swapInterval=%d)",
                 appliedTargetFps, appliedTargetFps <= 30 ? 2 : 1);
        }

        EGLint surfaceWidth = 0, surfaceHeight = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &surfaceWidth);
        eglQuerySurface(display, surface, EGL_HEIGHT, &surfaceHeight);
        const EGLint effSurfW = surfaceWidth > 0 ? surfaceWidth : 1920;
        const EGLint effSurfH = surfaceHeight > 0 ? surfaceHeight : 1080;

        GLint actX = 0, actY = 0;
        GLsizei actW = effSurfW, actH = effSurfH;
        computeActiveSurfaceRect(
            effSurfW, effSurfH,
            gMaintainAspectRatio16x9.load(std::memory_order_relaxed),
            actX, actY, actW, actH
        );

        bool hasFrame = D3D11Bridge_WaitForNextFrame(&frameState, 33);
        drainReadyPreuploads();
        if (!hasFrame) {
            const float logicalW = frameState.width > 0 ? static_cast<float>(frameState.width) : 1920.0f;
            const float logicalH = frameState.height > 0 ? static_cast<float>(frameState.height) : 1080.0f;
            processPendingFlushRequests(effSurfW, effSurfH, actX, actY, actW, actH, logicalW, logicalH);
            processFallbackPreuploads(5.0f);

            if (!hasReceivedFirstFrame) {
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                eglSwapBuffers(display, surface);
            }
            continue;
        }

        hasReceivedFirstFrame = true;
        processFallbackPreuploads(2.0f);
        glViewport(actX, actY, actW, actH);

        uint32_t activeW = 0, activeH = 0, activeRevision = 0;
        const uint8_t* activePixels = nullptr;
        bool hasActiveTexture = D3D11Bridge_GetActiveTextureInfo(&activeW, &activeH, &activePixels, &activeRevision);

        std::vector<MeshDrawCommand> frameMeshes;
        size_t meshCount = D3D11Bridge_SwapFrameMeshes(frameMeshes);

        bool hasBackbufferDraw = false;
        if (!frameMeshes.empty()) {
            for (const auto& cmd : frameMeshes) {
                if (cmd.renderTargetHandle == nullptr && cmd.isClearCommand == 0) {
                    hasBackbufferDraw = true;
                    break;
                }
            }
        } else if (hasActiveTexture && activePixels != nullptr && activeW == 1920 && activeH == 1080) {
            hasBackbufferDraw = true;
        }

        if (hasBackbufferDraw) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilMask(0xFF);
            // Luôn xóa toàn bộ bề mặt sang màu đen thuần để viền Pillarbox/Letterbox 16:9 sắc nét
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            if (frameState.clearColor[0] > 0.0f || frameState.clearColor[1] > 0.0f || frameState.clearColor[2] > 0.0f) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(actX, actY, actW, actH);
                glClearColor(
                    frameState.clearColor[0],
                    frameState.clearColor[1],
                    frameState.clearColor[2],
                    frameState.clearColor[3]
                );
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
        }

        const float logicalW = frameState.width > 0 ? static_cast<float>(frameState.width) : 1920.0f;
        const float logicalH = frameState.height > 0 ? static_cast<float>(frameState.height) : 1080.0f;

        if (meshCount > 0) {
            executeMeshCommandBatch(frameMeshes, effSurfW, effSurfH, actX, actY, actW, actH, logicalW, logicalH);
        } else if (hasActiveTexture && activePixels != nullptr && activeW == 1920 && activeH == 1080) {
            glViewport(actX, actY, actW, actH);
            glUseProgram(program);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1i(uTextureLoc, 0);
            glUniform1i(uHasAlphaTextureLoc, 0);
            glUniform1i(uHasDestTextureLoc, 0);
            float fallbackIdentityCb0[64] = {0};
            fallbackIdentityCb0[0] = 1.0f;
            fallbackIdentityCb0[5] = 1.0f;
            fallbackIdentityCb0[10] = 1.0f;
            fallbackIdentityCb0[15] = 1.0f;
            glUniform1i(uVsShaderIdLoc, 4);
            glUniform4fv(uVsCb0Loc, 16, fallbackIdentityCb0);
            glUniform2f(uUvScaleLoc, 1.0f, 1.0f);
            glUniform1f(uFlipYLoc, 1.0f);
            glUniform4f(uD3dViewportLoc, 0.0f, 0.0f, 1920.0f, 1080.0f);
            glUniform4f(uGlViewportLoc,
                        static_cast<float>(actX),
                        static_cast<float>(actY),
                        static_cast<float>(actW),
                        static_cast<float>(actH));
            glUniform1i(uShaderIdLoc, 180);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fallbackQuadTexture);
            static uint32_t sLastQuadRev = 0;
            if (currentTexWidth != activeW || currentTexHeight != activeH) {
                currentTexWidth = activeW;
                currentTexHeight = activeH;
                sLastQuadRev = activeRevision;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, activeW, activeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, activePixels);
            } else if (sLastQuadRev != activeRevision) {
                sLastQuadRev = activeRevision;
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, activeW, activeH, GL_RGBA, GL_UNSIGNED_BYTE, activePixels);
            }

            if (aPositionLoc >= 0) {
                glEnableVertexAttribArray(aPositionLoc);
                glVertexAttribPointer(aPositionLoc, 3, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &quadVertices[0]);
            }
            if (aTexCoord0Loc >= 0) {
                glEnableVertexAttribArray(aTexCoord0Loc);
                glVertexAttribPointer(aTexCoord0Loc, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &quadVertices[3]);
            }
            if (aColorLoc >= 0) {
                glEnableVertexAttribArray(aColorLoc);
                glVertexAttribPointer(aColorLoc, 4, GL_FLOAT, GL_FALSE, 11 * sizeof(float), &quadVertices[7]);
            }

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            if (aPositionLoc >= 0) glDisableVertexAttribArray(aPositionLoc);
            if (aTexCoord0Loc >= 0) glDisableVertexAttribArray(aTexCoord0Loc);
            if (aColorLoc >= 0) glDisableVertexAttribArray(aColorLoc);
            glDisable(GL_BLEND);
        }

        // Dọn dẹp an toàn các tài nguyên GLES của các handle đã bị giải phóng COM
        static std::vector<const void*> sReleasedHandles;
        D3D11Bridge_ConsumeReleasedHandles(sReleasedHandles);
        for (const void* deadHandle : sReleasedHandles) {
            releaseAllForHandle(deadHandle);
            delete reinterpret_cast<const SGRE_COM_Mock*>(deadHandle);
        }

        D3D11Bridge_NotifySpritesConsumed();

        if (hasBackbufferDraw) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            auto tSwapStart = std::chrono::steady_clock::now();
            eglSwapBuffers(display, surface);
            auto tSwapEnd = std::chrono::steady_clock::now();
            float swapMs = std::chrono::duration<float, std::milli>(tSwapEnd - tSwapStart).count();

            ++fpsFrameCount;
            float fpsWindowSec = std::chrono::duration<float>(tSwapEnd - fpsWindowStart).count();
            if (fpsWindowSec >= 0.5f) {
                gCurrentFps.store(static_cast<float>(fpsFrameCount) / fpsWindowSec, std::memory_order_relaxed);
                fpsFrameCount = 0;
                fpsWindowStart = tSwapEnd;
            }

            if (hasFrame) {
                D3D11Bridge_NotifyFramePresented();
            }

            static uint32_t sPresentCounter = 0;
            static auto sLastSwapEnd = std::chrono::steady_clock::now();
            constexpr float kFrameGapThresholdMs = 45.0f;
            constexpr uint32_t kPresentLogIntervalFrames = 60u;
            if (hasFrame) {
                ++sPresentCounter;
                // Do khoang tre giua hai frame render de truy vet diem khung chuyen canh
                float gapMs = std::chrono::duration<float, std::milli>(tSwapEnd - sLastSwapEnd).count();
                sLastSwapEnd = tSwapEnd;
                if (sPresentCounter > 1 && gapMs >= kFrameGapThresholdMs) {
                    LOGI("[PerfTrace] Frame %u gap=%.2fms (swap=%.2fms, drawIdx=%u, texCreated=%u)",
                         frameState.frameIndex, gapMs, swapMs,
                         frameState.indexDraws, frameState.texturesCreated);
                }
                if (sPresentCounter % kPresentLogIntervalFrames == 1) {
                    LOGI("EGL Present: Frame %u (%ux%u) | swap=%.2fms | ActiveTex: %dx%d (%s) | Color: (%.2f, %.2f, %.2f) | DrawIndexed: %u | Textures: %u | Shaders: %u",
                         frameState.frameIndex, frameState.width, frameState.height, swapMs,
                         activeW, activeH, hasActiveTexture ? "BOUND" : "NONE",
                         frameState.clearColor[0], frameState.clearColor[1], frameState.clearColor[2],
                         frameState.indexDraws, frameState.texturesCreated, frameState.shadersCreated);
                }
            }
        } else {
            if (hasFrame) {
                D3D11Bridge_NotifyFramePresented();
            }
        }

        // Kiểm tra ngay nếu Game Thread đã gửi yêu cầu readback ngay sau Present
        processPendingFlushRequests(effSurfW, effSurfH, actX, actY, actW, actH, logicalW, logicalH);
    }

    gCurrentFps.store(0.0f, std::memory_order_relaxed);
    D3D11Bridge_SetRenderLoopActive(false);
    preuploadRunning.store(false);
    D3D11Bridge_WakePreuploadThread();
    if (uploadThread.joinable()) {
        uploadThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(readyPreuploadMutex);
        for (const auto& item : readyPreuploads) {
            if (item.fence != nullptr) glDeleteSync(item.fence);
            if (item.texId != 0) glDeleteTextures(1, &item.texId);
        }
        readyPreuploads.clear();
    }

    // Bảo toàn các texture GPU-owned (MCapture / RenderTarget đang sống) về RAM CPU trước khi hủy EGLContext
    // giúp hình nền Pause Menu và Save Thumbnail không bị đen khi người dùng khóa màn hình hoặc thoát ra Home.
    {
        std::unordered_set<const void*> gpuHandlesToPreserve = sGpuCopiedHandles;
        gpuHandlesToPreserve.insert(sRenderTargetHandles.begin(), sRenderTargetHandles.end());
        uint32_t preservedCount = 0;
        for (const void* gpuHandle : gpuHandlesToPreserve) {
            auto itCached = sGlesTextureCache.find(gpuHandle);
            if (itCached == sGlesTextureCache.end() || itCached->second.texId == 0) continue;
            const uint32_t w = itCached->second.width;
            const uint32_t h = itCached->second.height;
            if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h * 4ull > 32ull * 1024 * 1024) continue;

            GLuint readFbo = getOrCreateCopyFbo(gpuHandle, w, h);
            if (readFbo == 0) continue;

            std::vector<uint8_t> readBuf(static_cast<size_t>(w) * h * 4u);
            while (glGetError() != GL_NO_ERROR) {}
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
            glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, readBuf.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            if (glGetError() == GL_NO_ERROR) {
                if (D3D11Bridge_PreserveGpuTextureToCpu(gpuHandle, readBuf.data(), readBuf.size())) {
                    ++preservedCount;
                }
            }
        }
        D3D11Bridge_ResetGpuUploadState();
        if (preservedCount > 0) {
            LOGI("Đã bảo toàn %u texture GPU-owned về RAM CPU trước khi hủy EGLContext", preservedCount);
        }
    }

    for (auto& pair : sGlesTextureCache) {
        if (pair.second.texId != 0) glDeleteTextures(1, &pair.second.texId);
    }
    sGlesTextureCache.clear();
    clearRecyclePool();
    if (streamVbo != 0) glDeleteBuffers(1, &streamVbo);
    if (streamIbo != 0) glDeleteBuffers(1, &streamIbo);
    if (sBackbufferFlipTempTex != 0) {
        glDeleteTextures(1, &sBackbufferFlipTempTex);
    }
    for (auto& pair : sFboCache) {
        if (pair.second != 0) {
            glDeleteFramebuffers(1, &pair.second);
        }
    }
    sFboCache.clear();
    for (auto& pair : sCopyFboCache) {
        if (pair.second != 0) {
            glDeleteFramebuffers(1, &pair.second);
        }
    }
    sCopyFboCache.clear();
    for (auto& pair : sStencilRboCache) {
        if (pair.second != 0) {
            glDeleteRenderbuffers(1, &pair.second);
        }
    }
    sStencilRboCache.clear();
    sRenderTargetHandles.clear();
    sGpuCopiedHandles.clear();

    if (whitePixelTexture != 0) {
        glDeleteTextures(1, &whitePixelTexture);
    }
    if (fallbackQuadTexture != 0) {
        glDeleteTextures(1, &fallbackQuadTexture);
    }
    sgre::ShaderCache::destroyProgramSet(programSet);

    LOGI("Dừng vòng lặp render, giải phóng EGL...");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (uploadContext != EGL_NO_CONTEXT) {
        eglDestroyContext(display, uploadContext);
    }
    if (uploadPbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(display, uploadPbuffer);
    }
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

    std::lock_guard<std::mutex> lifecycleLock(gLifecycleMutex);

    if (gIsRunning.load()) {
        gIsRunning.store(false);
        D3D11Bridge_WakeRenderThread();
        if (gRenderThread.joinable()) {
            gRenderThread.join();
        }
    }

    if (gNativeWindow != nullptr) {
        ANativeWindow_release(gNativeWindow);
        gNativeWindow = nullptr;
    }

    if (surface != nullptr) {
        gNativeWindow = ANativeWindow_fromSurface(env, surface);
    }

    if (dataPath != nullptr) {
        const char* pathStr = env->GetStringUTFChars(dataPath, nullptr);
        if (pathStr != nullptr) {
            gDataDirectory = pathStr;
            env->ReleaseStringUTFChars(dataPath, pathStr);
        }
    }

    LOGI("Native Init hoàn tất, data dir: %s", gDataDirectory.c_str());
    Win32Bridge_SetDataDirectory(gDataDirectory.c_str());
    SteamHook_SetDataDirectory(gDataDirectory.c_str());

    // Chỉ nạp PE binary và khởi chạy luồng thực thi game duy nhất một lần trong suốt vòng đời tiến trình!
    if (!gEngineLaunched.exchange(true)) {
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

                std::thread gameThread([loadedBase, payloadInfo]() {
                    PayloadLoader::executePayload(loadedBase, payloadInfo);
                });
                gameThread.detach();
            } else {
                gEngineLaunched.store(false);
            }
        } else {
            gEngineLaunched.store(false);
            LOGE("Chưa tìm thấy hoặc binary sgre_steam.exe không hợp lệ");
        }
    } else {
        LOGI("Engine đã đang chạy nền, tái kết nối bề mặt ANativeWindow mới vào luồng Render...");
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
Java_com_makurisu_sgrb_MainActivity_nativeSendKeyEvent(
    JNIEnv* /* env */,
    jclass /* clazz */,
    jint keyCode,
    jboolean isDown) {
    InputInjector::injectKeyEvent(static_cast<int32_t>(keyCode), isDown == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeSendGamepadAxes(
    JNIEnv* /* env */,
    jclass /* clazz */,
    jfloat leftX,
    jfloat leftY,
    jfloat rightX,
    jfloat rightY,
    jfloat triggerL,
    jfloat triggerR,
    jfloat hatX,
    jfloat hatY) {
    XInputBridge_UpdateAxes(leftX, leftY, rightX, rightY, triggerL, triggerR, hatX, hatY);
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeSetAspectRatio16x9(
    JNIEnv* /* env */,
    jclass /* clazz */,
    jboolean maintain16x9) {
    gMaintainAspectRatio16x9.store(maintain16x9 == JNI_TRUE, std::memory_order_relaxed);
    D3D11Bridge_WakeRenderThread();
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeSetTargetFps(
    JNIEnv* /* env */,
    jclass /* clazz */,
    jint targetFps) {
    const int32_t clamped = (targetFps <= 30) ? 30 : 60;
    gTargetFps.store(clamped, std::memory_order_relaxed);
    D3D11Bridge_WakeRenderThread();
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeSetLanguageOverride(
    JNIEnv* env,
    jclass /* clazz */,
    jstring langCode) {
    if (langCode == nullptr) {
        SteamHook_SetLanguageOverride("");
        return;
    }
    const char* raw = env->GetStringUTFChars(langCode, nullptr);
    if (raw != nullptr) {
        SteamHook_SetLanguageOverride(raw);
        env->ReleaseStringUTFChars(langCode, raw);
    }
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeGetCurrentFps(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    return static_cast<jfloat>(gCurrentFps.load(std::memory_order_relaxed));
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeGetCpuMemoryMb(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    const double bytes = static_cast<double>(D3D11Bridge_GetCpuMemoryBytes());
    return static_cast<jfloat>(bytes / (1024.0 * 1024.0));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeGetCpuTranslationStatus(
    JNIEnv* env,
    jclass /* clazz */,
    jstring dataPath) {
    std::string dir;
    if (dataPath != nullptr) {
        const char* raw = env->GetStringUTFChars(dataPath, nullptr);
        if (raw != nullptr) {
            dir = raw;
            env->ReleaseStringUTFChars(dataPath, raw);
        }
    }
    return static_cast<jint>(sgre::Arm64Box64Bridge::probeRuntime(dir));
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativePause(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    AudioSink_SetLifecyclePaused(true);
    XAudio2Bridge_SetLifecyclePaused(true);
    Win32Bridge_PostMessage(0x001C /* WM_ACTIVATEAPP */, 0, 0);
    LOGI("Đã tạm dừng âm thanh và gửi thông điệp WM_ACTIVATEAPP(0) khi ứng dụng xuống nền");
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeResume(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    AudioSink_SetLifecyclePaused(false);
    XAudio2Bridge_SetLifecyclePaused(false);
    Win32Bridge_PostMessage(0x001C /* WM_ACTIVATEAPP */, 1, 0);
    D3D11Bridge_WakeRenderThread();
    LOGI("Đã khôi phục âm thanh và gửi thông điệp WM_ACTIVATEAPP(1) khi ứng dụng trở lại");
}

extern "C" JNIEXPORT void JNICALL
Java_com_makurisu_sgrb_MainActivity_nativeDestroy(
    JNIEnv* /* env */,
    jclass /* clazz */) {

    std::lock_guard<std::mutex> lifecycleLock(gLifecycleMutex);
    if (gIsRunning.load()) {
        gIsRunning.store(false);
        D3D11Bridge_WakeRenderThread();
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

