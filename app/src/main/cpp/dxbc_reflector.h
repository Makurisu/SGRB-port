#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sgre {

struct DxbcTypeInfo {
    uint32_t varClass = 1;  // 0 = Scalar, 1 = Vector, 3 = Matrix_Columns
    uint32_t varType = 3;   // 3 = Float
    uint32_t rows = 1;
    uint32_t columns = 4;
    uint32_t elements = 1;  // Luôn >= 1 theo yêu cầu của M2 Engine (sub_140565020)
    uint32_t members = 0;
    uint32_t offset = 0;
    std::string name = "float4";
};

struct DxbcVariableInfo {
    std::string name;
    uint32_t startOffset = 0;
    uint32_t size = 0;
    uint32_t uFlags = 0;
    DxbcTypeInfo typeInfo;
};

struct DxbcConstantBufferInfo {
    std::string name;
    uint32_t type = 0;  // 0 = D3D_CT_CBUFFER
    uint32_t size = 0;
    uint32_t uFlags = 0;
    std::vector<DxbcVariableInfo> variables;
};

struct DxbcResourceBindingInfo {
    std::string name;
    uint32_t type = 0;        // 0 = CBuffer, 2 = Texture, 3 = Sampler
    uint32_t returnType = 0;
    uint32_t dimension = 0;
    uint32_t numSamples = 0;
    uint32_t bindPoint = 0;
    uint32_t bindCount = 1;
    uint32_t uFlags = 0;
};

struct DxbcReflectionData {
    uint32_t version = 0x500;
    std::string creator = "M2 Engine / Direct3D 11";
    uint32_t flags = 0;
    std::vector<DxbcConstantBufferInfo> constantBuffers;
    std::vector<DxbcResourceBindingInfo> boundResources;
};

// Giải mã trực tiếp chunk RDEF từ DXBC bytecode chuẩn DirectX 11.
// Trả về true nếu parse thành công, false nếu dữ liệu không hợp lệ hoặc bị cắt cụt.
bool parseDxbcRdef(const void* bytecode, size_t bytecodeSize, DxbcReflectionData& outData);

// Ánh xạ Vertex Shader ID (theo thứ tự khởi tạo 0..11 từ bảng unk_140654880 trong sub_14054CDE0)
// sang các thuộc tính cấu hình tương ứng.
inline bool isVertexShader3D(uint32_t vsShaderId) {
    return vsShaderId >= 6 && vsShaderId < 12;
}

inline bool isVertexShaderUsingConstColor(uint32_t vsShaderId) {
    // Trong sub_14054CDE0 (unk_140654880):
    // idx 0: (0,1,0), idx 1: (0,0,0), idx 2: (0,1,1), idx 3: (0,1,2), idx 4: (0,0,1), idx 5: (0,0,2)
    // idx 6: (1,1,0), idx 7: (1,0,0), idx 8: (1,1,1), idx 9: (1,1,2), idx 10:(1,0,1), idx 11:(1,0,2)
    return vsShaderId == 0 || vsShaderId == 2 || vsShaderId == 3 ||
           vsShaderId == 6 || vsShaderId == 8 || vsShaderId == 9;
}

inline uint32_t getVertexShaderTexCoordCount(uint32_t vsShaderId) {
    switch (vsShaderId) {
        case 0:
        case 1:
        case 6:
        case 7:
            return 0;
        case 2:
        case 4:
        case 8:
        case 10:
            return 1;
        case 3:
        case 5:
        case 9:
        case 11:
            return 2;
        default:
            return 1;
    }
}

}  // namespace sgre
