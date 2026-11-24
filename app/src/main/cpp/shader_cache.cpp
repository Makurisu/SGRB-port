#include "shader_cache.h"
#include <android/log.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <vector>

#define LOG_TAG "SGRE_ShaderCache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace sgre {

namespace {

constexpr uint32_t kShaderBinaryMagic = 0x53475342u; // 'SGSB'
constexpr uint32_t kShaderCacheSchemaVersion = 2u;

struct ShaderBinaryHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t sourceHash;
    uint32_t binaryFormat;
    uint32_t payloadSize;
};

static uint64_t hashFnv1a64(const char* str, uint64_t seed = 14695981039346656037ull) {
    if (!str) return seed;
    uint64_t h = seed;
    while (*str) {
        h ^= static_cast<uint8_t>(*str++);
        h *= 1099511628211ull;
    }
    return h;
}

// Họ 0: Standard Sprite (uShaderId < 900) - gọn nhẹ, loại bỏ hoàn toàn sampler t2 và nhánh blur 19 chế độ
static const char* kStandardSpriteFragSource =
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
    "uniform int uShaderId;\n"
    "uniform vec4 uPsCb0[8];\n"
    "uniform float uFlipY;\n"
    "uniform vec4 uD3dViewport;\n"
    "uniform vec4 uGlViewport;\n"
    "void main() {\n"
    "    vec4 baseColor = texture2D(uTexture, vTexCoord);\n"
    "    int colorMode = uShaderId / 60;\n"
    "    int rem = uShaderId - colorMode * 60;\n"
    "    int maskMode = rem / 15;\n"
    "    int rem15 = rem - maskMode * 15;\n"
    "    int filterMode = rem15 / 3;\n"
    "    int outMode = rem15 - filterMode * 3;\n"
    "    vec4 c;\n"
    "    if (colorMode == 4) {\n"
    "        c = baseColor * vColor;\n"
    "    } else if (colorMode == 3) {\n"
    "        c = baseColor;\n"
    "    } else if (colorMode == 0) {\n"
    "        c = vColor;\n"
    "    } else if (colorMode == 1) {\n"
    "        c = vColor * vColor;\n"
    "    } else if (colorMode == 2) {\n"
    "        vec4 v2 = vColor * vColor;\n"
    "        c = vec4(v2.rgb * 2.0, v2.a);\n"
    "    } else if (colorMode == 5) {\n"
    "        vec4 m = baseColor * vColor;\n"
    "        c = vec4(clamp(m.rgb * 2.0, 0.0, 1.0), m.a);\n"
    "    } else if (colorMode == 6) {\n"
    "        c = vec4(1.0, 1.0, 1.0, baseColor.a);\n"
    "    } else if (colorMode == 7) {\n"
    "        c = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
    "    } else if (colorMode == 8) {\n"
    "        vec4 m = vec4(1.0, 1.0, 1.0, baseColor.a) * vColor;\n"
    "        c = vec4(m.rgb * 2.0, m.a);\n"
    "    } else if (colorMode == 9) {\n"
    "        c = vec4(baseColor.rrr, 1.0);\n"
    "    } else if (colorMode == 10) {\n"
    "        c = vec4(baseColor.rrr, 1.0) * vColor;\n"
    "    } else if (colorMode == 11) {\n"
    "        vec4 m = vec4(baseColor.rrr, 1.0) * vColor;\n"
    "        c = vec4(m.rgb * 2.0, m.a);\n"
    "    } else if (colorMode == 12) {\n"
    "        c = vec4(baseColor.rrr, baseColor.g);\n"
    "    } else if (colorMode == 13) {\n"
    "        c = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
    "    } else {\n"
    "        vec4 m = vec4(baseColor.rrr, baseColor.g) * vColor;\n"
    "        c = vec4(m.rgb * 2.0, m.a);\n"
    "    }\n"
    "    if (filterMode != 0) {\n"
    "        vec4 fCol = (maskMode == 1) ? uPsCb0[1] : uPsCb0[0];\n"
    "        if (filterMode == 1) c.rgb = mix(c.rgb, fCol.rgb, fCol.a);\n"
    "        else if (filterMode == 2) c.rgb = c.rgb + fCol.rgb * fCol.a;\n"
    "        else if (filterMode == 3) c.rgb = c.rgb - fCol.rgb * fCol.a;\n"
    "        else if (filterMode == 4) c.rgb = c.rgb + (vec3(1.0) - 2.0 * c.rgb) * fCol.a;\n"
    "    }\n"
    "    if (uHasAlphaTexture == 1) {\n"
    "        if (maskMode == 1) {\n"
    "            vec2 normPos = vec2(\n"
    "                (gl_FragCoord.x - uGlViewport.x) / uGlViewport.z,\n"
    "                (uFlipY > 0.0) ? (1.0 - (gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
    "                               : ((gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
    "            );\n"
    "            vec2 svPos = uD3dViewport.xy + normPos * uD3dViewport.zw;\n"
    "            vec2 maskUv = (uPsCb0[0].x > 0.0 && uPsCb0[0].y > 0.0) ? (svPos * uPsCb0[0].xy) : normPos;\n"
    "            c.a *= texture2D(uAlphaTexture, maskUv).a;\n"
    "        } else if (maskMode == 2) {\n"
    "            float ma = texture2D(uAlphaTexture, vTexCoord).a;\n"
    "            if (c.a * ma < 0.001961) discard;\n"
    "            c.a *= ma;\n"
    "        } else if (maskMode == 3) {\n"
    "            c.a *= texture2D(uAlphaTexture, vTexCoord1).a;\n"
    "        }\n"
    "    }\n"
    "    if (outMode == 1) c.rgb = vec3(1.0) - c.rgb;\n"
    "    else if (outMode == 2) c.rgb *= c.a;\n"
    "    gl_FragColor = c;\n"
    "}\n";

// Họ 1: Wipe Transition & Utility (2040 .. 2076)
static const char* kWipeUtilityFragSource =
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
    "uniform int uShaderId;\n"
    "uniform vec4 uPsCb0[8];\n"
    "uniform float uFlipY;\n"
    "uniform vec4 uD3dViewport;\n"
    "uniform vec4 uGlViewport;\n"
    "void main() {\n"
    "    vec4 baseColor = texture2D(uTexture, vTexCoord);\n"
    "    vec4 uCb0 = uPsCb0[0];\n"
    "    if (uShaderId == 2040) {\n"
    "        float maskVal = (uHasAlphaTexture == 1) ? texture2D(uAlphaTexture, vTexCoord1).r : 1.0;\n"
    "        float effAlpha = clamp((maskVal - uCb0.x) * uCb0.y + 1.0, 0.0, 1.0) * vColor.a;\n"
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
    "        if (grp == 0) { outRgb = vColor.rgb; rawA = vColor.a; }\n"
    "        else if (grp == 1) { outRgb = baseColor.rgb; rawA = baseColor.a; }\n"
    "        else if (grp == 2) { outRgb = vec3(1.0); rawA = baseColor.a; }\n"
    "        else if (grp == 3) { outRgb = baseColor.rrr; rawA = 1.0; }\n"
    "        else { outRgb = baseColor.rrr; rawA = baseColor.g; }\n"
    "        float effAlpha = (sub >= 3) ? clamp(rawA * uCb0.x + uCb0.y, 0.0, 1.0) : rawA;\n"
    "        if ((sub == 1 || sub == 4) && uHasAlphaTexture == 1) {\n"
    "            vec2 normPos = vec2(\n"
    "                (gl_FragCoord.x - uGlViewport.x) / uGlViewport.z,\n"
    "                (uFlipY > 0.0) ? (1.0 - (gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
    "                               : ((gl_FragCoord.y - uGlViewport.y) / uGlViewport.w)\n"
    "            );\n"
    "            vec2 svPos = uD3dViewport.xy + normPos * uD3dViewport.zw;\n"
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
    "    } else {\n"
    "        if (uCb0.x >= baseColor.g) discard;\n"
    "        gl_FragColor = vec4(baseColor.rrr, baseColor.g);\n"
    "    }\n"
    "}\n";

static GLuint compileSingleShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            std::string infoLog(static_cast<size_t>(infoLen), '\0');
            glGetShaderInfoLog(shader, infoLen, nullptr, &infoLog[0]);
            LOGE("Biên dịch GLSL shader (%u) thất bại: %s", type, infoLog.c_str());
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void populateLocations(GlProgramBinding& b) {
    if (b.program == 0) return;
    b.aPositionLoc = glGetAttribLocation(b.program, "aPosition");
    b.aTexCoord0Loc = glGetAttribLocation(b.program, "aTexCoord0");
    b.aTexCoord1Loc = glGetAttribLocation(b.program, "aTexCoord1");
    b.aColorLoc = glGetAttribLocation(b.program, "aColor");
    b.uTextureLoc = glGetUniformLocation(b.program, "uTexture");
    b.uAlphaTextureLoc = glGetUniformLocation(b.program, "uAlphaTexture");
    b.uHasAlphaTextureLoc = glGetUniformLocation(b.program, "uHasAlphaTexture");
    b.uDestTextureLoc = glGetUniformLocation(b.program, "uDestTexture");
    b.uHasDestTextureLoc = glGetUniformLocation(b.program, "uHasDestTexture");
    b.uVsShaderIdLoc = glGetUniformLocation(b.program, "uVsShaderId");
    b.uVsCb0Loc = glGetUniformLocation(b.program, "uVsCb0[0]");
    if (b.uVsCb0Loc < 0) b.uVsCb0Loc = glGetUniformLocation(b.program, "uVsCb0");
    b.uShaderIdLoc = glGetUniformLocation(b.program, "uShaderId");
    b.uPsCb0Loc = glGetUniformLocation(b.program, "uPsCb0[0]");
    if (b.uPsCb0Loc < 0) b.uPsCb0Loc = glGetUniformLocation(b.program, "uPsCb0");
    b.uUvScaleLoc = glGetUniformLocation(b.program, "uUvScale");
    b.uFlipYLoc = glGetUniformLocation(b.program, "uFlipY");
    b.uD3dViewportLoc = glGetUniformLocation(b.program, "uD3dViewport");
    b.uGlViewportLoc = glGetUniformLocation(b.program, "uGlViewport");

    glUseProgram(b.program);
    if (b.uTextureLoc >= 0) glUniform1i(b.uTextureLoc, 0);
    if (b.uAlphaTextureLoc >= 0) glUniform1i(b.uAlphaTextureLoc, 1);
    if (b.uDestTextureLoc >= 0) glUniform1i(b.uDestTextureLoc, 2);
    if (b.uUvScaleLoc >= 0) glUniform2f(b.uUvScaleLoc, 1.0f, 1.0f);
}

static GLuint tryLoadProgramBinary(const std::string& filePath, uint64_t expectedHash) {
    FILE* fp = std::fopen(filePath.c_str(), "rb");
    if (!fp) return 0;

    ShaderBinaryHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != kShaderBinaryMagic ||
        hdr.version != kShaderCacheSchemaVersion ||
        hdr.sourceHash != expectedHash ||
        hdr.payloadSize == 0 ||
        hdr.payloadSize > 4u * 1024u * 1024u) {
        std::fclose(fp);
        return 0;
    }

    std::vector<uint8_t> payload(hdr.payloadSize);
    if (std::fread(payload.data(), 1, hdr.payloadSize, fp) != hdr.payloadSize) {
        std::fclose(fp);
        return 0;
    }
    std::fclose(fp);

    GLuint prog = glCreateProgram();
    glProgramBinary(prog, static_cast<GLenum>(hdr.binaryFormat), payload.data(), static_cast<GLsizei>(payload.size()));
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void saveProgramBinary(const std::string& filePath, GLuint prog, uint64_t sourceHash) {
    GLint binLen = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &binLen);
    if (binLen <= 0 || binLen > 4 * 1024 * 1024) return;

    std::vector<uint8_t> payload(static_cast<size_t>(binLen));
    GLsizei actualLen = 0;
    GLenum binFormat = 0;
    glGetProgramBinary(prog, binLen, &actualLen, &binFormat, payload.data());
    if (actualLen <= 0 || static_cast<size_t>(actualLen) > payload.size()) return;

    FILE* fp = std::fopen(filePath.c_str(), "wb");
    if (!fp) return;

    ShaderBinaryHeader hdr{};
    hdr.magic = kShaderBinaryMagic;
    hdr.version = kShaderCacheSchemaVersion;
    hdr.sourceHash = sourceHash;
    hdr.binaryFormat = static_cast<uint32_t>(binFormat);
    hdr.payloadSize = static_cast<uint32_t>(actualLen);

    std::fwrite(&hdr, sizeof(hdr), 1, fp);
    std::fwrite(payload.data(), 1, static_cast<size_t>(actualLen), fp);
    std::fclose(fp);
}

static GLuint buildOrLoadFamilyProgram(
    const std::string& cacheDir,
    size_t familyIdx,
    GLuint sharedVertShader,
    const char* vsSrc,
    const char* fsSrc,
    const char* rendererSig,
    bool canUseBinaryCache
) {
    uint64_t hash = hashFnv1a64(vsSrc);
    hash = hashFnv1a64(fsSrc, hash);
    hash = hashFnv1a64(rendererSig, hash);

    std::string binPath;
    if (canUseBinaryCache && !cacheDir.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/prog_f%zu_%016llx.bin", familyIdx, static_cast<unsigned long long>(hash));
        binPath = cacheDir + buf;
        GLuint cachedProg = tryLoadProgramBinary(binPath, hash);
        if (cachedProg != 0) {
            LOGI("Đã nạp nhanh Shader Family #%zu từ bộ nhớ đệm nhị phân trên đĩa", familyIdx);
            return cachedProg;
        }
    }

    GLuint fs = compileSingleShader(GL_FRAGMENT_SHADER, fsSrc);
    if (fs == 0) return 0;

    GLuint prog = glCreateProgram();
    if (canUseBinaryCache) {
        glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glAttachShader(prog, sharedVertShader);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDetachShader(prog, sharedVertShader);
    glDetachShader(prog, fs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOGW("glLinkProgram thất bại cho Shader Family #%zu", familyIdx);
        glDeleteProgram(prog);
        return 0;
    }

    if (canUseBinaryCache && !binPath.empty()) {
        saveProgramBinary(binPath, prog, hash);
    }
    return prog;
}

} // namespace

ShaderProgramSet ShaderCache::buildProgramSet(
    const std::string& dataDirectory,
    const char* vertexShaderSource,
    const char* uberFragmentShaderSource,
    bool isGles3Context
) {
    ShaderProgramSet set{};
    if (!vertexShaderSource || !uberFragmentShaderSource) {
        return set;
    }

    GLint numBinaryFormats = 0;
    if (isGles3Context) {
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numBinaryFormats);
    }
    const bool canUseBinaryCache = isGles3Context && (numBinaryFormats > 0) && !dataDirectory.empty();

    std::string cacheDir;
    if (canUseBinaryCache) {
        cacheDir = dataDirectory + "/.shader_cache";
        ::mkdir(cacheDir.c_str(), 0775);
    }

    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::string rendererSig = std::string(vendor ? vendor : "") + "|" +
                              std::string(renderer ? renderer : "") + "|" +
                              std::string(version ? version : "");

    GLuint sharedVs = compileSingleShader(GL_VERTEX_SHADER, vertexShaderSource);
    if (sharedVs == 0) {
        return set;
    }

    const char* familySources[static_cast<size_t>(ShaderFamily::Count)] = {
        kStandardSpriteFragSource,
        kWipeUtilityFragSource,
        uberFragmentShaderSource,
        uberFragmentShaderSource
    };

    // Biên dịch UberFallback (3) trước để dùng chung cho ProgrammableBlend (2) nhằm tiết kiệm biên dịch trùng lặp
    GLuint uberProg = buildOrLoadFamilyProgram(
        cacheDir,
        static_cast<size_t>(ShaderFamily::UberFallback),
        sharedVs,
        vertexShaderSource,
        uberFragmentShaderSource,
        rendererSig.c_str(),
        canUseBinaryCache
    );
    set.families[static_cast<size_t>(ShaderFamily::UberFallback)].program = uberProg;
    populateLocations(set.families[static_cast<size_t>(ShaderFamily::UberFallback)]);

    // ProgrammableBlend chia sẻ cùng chương trình đã biên dịch với UberFallback (tránh glDeleteProgram hai lần khi hủy)
    set.families[static_cast<size_t>(ShaderFamily::ProgrammableBlend)] =
        set.families[static_cast<size_t>(ShaderFamily::UberFallback)];

    for (size_t idx = 0; idx < static_cast<size_t>(ShaderFamily::ProgrammableBlend); ++idx) {
        GLuint p = buildOrLoadFamilyProgram(
            cacheDir, idx, sharedVs, vertexShaderSource, familySources[idx], rendererSig.c_str(), canUseBinaryCache
        );
        set.families[idx].program = p;
        populateLocations(set.families[idx]);
    }

    glDeleteShader(sharedVs);
    LOGI("ShaderProgramSet sẵn sàng: Standard=%u, Wipe=%u, Uber=%u (BinaryCache=%s)",
         set.families[0].program, set.families[1].program, uberProg, canUseBinaryCache ? "ON" : "OFF");
    return set;
}

void ShaderCache::destroyProgramSet(ShaderProgramSet& set) {
    GLuint deletedProgs[4] = {0, 0, 0, 0};
    size_t deletedCount = 0;
    for (size_t i = 0; i < static_cast<size_t>(ShaderFamily::Count); ++i) {
        GLuint p = set.families[i].program;
        if (p != 0) {
            bool alreadyDeleted = false;
            for (size_t d = 0; d < deletedCount; ++d) {
                if (deletedProgs[d] == p) {
                    alreadyDeleted = true;
                    break;
                }
            }
            if (!alreadyDeleted) {
                glDeleteProgram(p);
                deletedProgs[deletedCount++] = p;
            }
        }
        set.families[i] = GlProgramBinding{};
    }
}

} // namespace sgre
