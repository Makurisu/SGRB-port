#pragma once
#include <GLES3/gl3.h>
#include <cstdint>
#include <string>

namespace sgre {

enum class ShaderFamily : uint8_t {
    StandardSprite = 0,     // uShaderId < 900 (hơn 92% lệnh vẽ sprite/UI/nhân vật)
    WipeAndUtility = 1,     // uShaderId >= 2040 (chuyển cảnh wipe, alpha mul-add, discard)
    ProgrammableBlend = 2,  // 900 <= uShaderId < 2040 (19 chế độ hòa trộn + Gaussian blur)
    UberFallback = 3,       // Shader tổng quát dự phòng
    Count = 4
};

struct GlProgramBinding {
    GLuint program = 0;
    GLint aPositionLoc = -1;
    GLint aTexCoord0Loc = -1;
    GLint aTexCoord1Loc = -1;
    GLint aColorLoc = -1;
    GLint uTextureLoc = -1;
    GLint uAlphaTextureLoc = -1;
    GLint uHasAlphaTextureLoc = -1;
    GLint uDestTextureLoc = -1;
    GLint uHasDestTextureLoc = -1;
    GLint uVsShaderIdLoc = -1;
    GLint uVsCb0Loc = -1;
    GLint uShaderIdLoc = -1;
    GLint uPsCb0Loc = -1;
    GLint uUvScaleLoc = -1;
    GLint uFlipYLoc = -1;
    GLint uD3dViewportLoc = -1;
    GLint uGlViewportLoc = -1;
};

struct ShaderProgramSet {
    GlProgramBinding families[static_cast<size_t>(ShaderFamily::Count)];

    const GlProgramBinding& selectForShaderId(uint32_t shaderId) const {
        ShaderFamily fam = classifyShaderId(shaderId);
        const GlProgramBinding& candidate = families[static_cast<size_t>(fam)];
        if (candidate.program != 0) {
            return candidate;
        }
        return families[static_cast<size_t>(ShaderFamily::UberFallback)];
    }

    static ShaderFamily classifyShaderId(uint32_t shaderId) {
        if (shaderId < 900u) {
            return ShaderFamily::StandardSprite;
        }
        if (shaderId >= 2040u && shaderId <= 2076u) {
            return ShaderFamily::WipeAndUtility;
        }
        if (shaderId >= 900u && shaderId < 2040u) {
            return ShaderFamily::ProgrammableBlend;
        }
        return ShaderFamily::UberFallback;
    }
};

class ShaderCache {
public:
    // Biên dịch hoặc nạp từ bộ nhớ đệm nhị phân trên đĩa (.shader_cache) bộ 4 chương trình GLSL chuyên biệt
    static ShaderProgramSet buildProgramSet(
        const std::string& dataDirectory,
        const char* vertexShaderSource,
        const char* uberFragmentShaderSource,
        bool isGles3Context
    );

    static void destroyProgramSet(ShaderProgramSet& set);
};

} // namespace sgre
