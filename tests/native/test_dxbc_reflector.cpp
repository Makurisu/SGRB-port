// Test native chạy trên host kiểm tra bộ giải mã DXBC RDEF (dxbc_reflector)
// và bảng ánh xạ 12 Vertex Shaders.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "dxbc_reflector.h"

using namespace sgre;

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++gChecks;                                                                 \
        if (!(cond)) {                                                             \
            ++gFailures;                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                          \
    } while (0)

static std::vector<uint8_t> readShaderFile(const std::string& filename) {
    static constexpr const char* kCandidateDirs[] = {"extracted_shaders/", "../../extracted_shaders/"};
    std::string fullPath = std::string(kCandidateDirs[0]) + filename;
    if (!std::ifstream(fullPath, std::ios::binary).is_open()) {
        fullPath = std::string(kCandidateDirs[1]) + filename;
    }
    std::ifstream ifs(fullPath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return {};
    }
    std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}

static void testInvalidAndEdgeCaseInputs() {
    DxbcReflectionData data{};
    CHECK(!parseDxbcRdef(nullptr, 100, data));

    uint8_t shortBuf[16] = {'D', 'X', 'B', 'C'};
    CHECK(!parseDxbcRdef(shortBuf, sizeof(shortBuf), data));

    uint8_t badMagic[64] = {'B', 'A', 'D', '!'};
    CHECK(!parseDxbcRdef(badMagic, sizeof(badMagic), data));

    // Bytecode giả mạo có totalSize vượt quá kích thước mảng truyền vào
    uint8_t truncated[64] = {};
    std::memcpy(truncated, "DXBC", 4);
    uint32_t fakeSize = 1024;
    std::memcpy(truncated + 24, &fakeSize, 4);
    CHECK(!parseDxbcRdef(truncated, sizeof(truncated), data));
}

static void testVertexShadersRdef() {
    // vs_00: chỉ có modelViewProj (offset 0, size 64)
    auto vs00 = readShaderFile("vs_00.bin");
    CHECK(!vs00.empty());
    DxbcReflectionData r00{};
    CHECK(parseDxbcRdef(vs00.data(), vs00.size(), r00));
    CHECK(r00.constantBuffers.size() == 1);
    CHECK(r00.constantBuffers[0].name == "$Globals");
    CHECK(r00.constantBuffers[0].size == 64);
    CHECK(r00.constantBuffers[0].variables.size() == 1);
    CHECK(r00.constantBuffers[0].variables[0].name == "modelViewProj");
    CHECK(r00.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r00.constantBuffers[0].variables[0].size == 64);
    CHECK(r00.constantBuffers[0].variables[0].typeInfo.rows == 4);
    CHECK(r00.constantBuffers[0].variables[0].typeInfo.columns == 4);
    CHECK(r00.constantBuffers[0].variables[0].typeInfo.elements == 1);

    // vs_03: có modelViewProj (0, 64) và constVtxColor (64, 16)
    auto vs03 = readShaderFile("vs_03.bin");
    CHECK(!vs03.empty());
    DxbcReflectionData r03{};
    CHECK(parseDxbcRdef(vs03.data(), vs03.size(), r03));
    CHECK(r03.constantBuffers.size() == 1);
    CHECK(r03.constantBuffers[0].size == 80);
    CHECK(r03.constantBuffers[0].variables.size() == 2);
    CHECK(r03.constantBuffers[0].variables[0].name == "modelViewProj");
    CHECK(r03.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r03.constantBuffers[0].variables[1].name == "constVtxColor");
    CHECK(r03.constantBuffers[0].variables[1].startOffset == 64);
    CHECK(r03.constantBuffers[0].variables[1].size == 16);
}

static void testPixelShadersRdef() {
    // ps_0000: không có Constant Buffer và không có Texture
    auto ps0000 = readShaderFile("ps_0000.bin");
    CHECK(!ps0000.empty());
    DxbcReflectionData r0000{};
    CHECK(parseDxbcRdef(ps0000.data(), ps0000.size(), r0000));
    CHECK(r0000.constantBuffers.empty());
    CHECK(r0000.boundResources.empty());

    // ps_0195: screenMul @ offset 0, size 8
    auto ps0195 = readShaderFile("ps_0195.bin");
    CHECK(!ps0195.empty());
    DxbcReflectionData r0195{};
    CHECK(parseDxbcRdef(ps0195.data(), ps0195.size(), r0195));
    CHECK(r0195.constantBuffers.size() == 1);
    CHECK(r0195.constantBuffers[0].size == 16);
    CHECK(r0195.constantBuffers[0].variables.size() == 1);
    CHECK(r0195.constantBuffers[0].variables[0].name == "screenMul");
    CHECK(r0195.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r0195.constantBuffers[0].variables[0].size == 8);

    // ps_0198: screenMul @ offset 0 (size 8), filterColor @ offset 16 (size 16)
    auto ps0198 = readShaderFile("ps_0198.bin");
    CHECK(!ps0198.empty());
    DxbcReflectionData r0198{};
    CHECK(parseDxbcRdef(ps0198.data(), ps0198.size(), r0198));
    CHECK(r0198.constantBuffers.size() == 1);
    CHECK(r0198.constantBuffers[0].size == 32);
    CHECK(r0198.constantBuffers[0].variables.size() == 2);
    CHECK(r0198.constantBuffers[0].variables[0].name == "screenMul");
    CHECK(r0198.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r0198.constantBuffers[0].variables[1].name == "filterColor");
    CHECK(r0198.constantBuffers[0].variables[1].startOffset == 16);
    CHECK(r0198.constantBuffers[0].variables[1].size == 16);

    // ps_2040: phase @ offset 0 (size 4), invVague @ offset 4 (size 4)
    auto ps2040 = readShaderFile("ps_2040.bin");
    CHECK(!ps2040.empty());
    DxbcReflectionData r2040{};
    CHECK(parseDxbcRdef(ps2040.data(), ps2040.size(), r2040));
    CHECK(r2040.constantBuffers.size() == 1);
    CHECK(r2040.constantBuffers[0].size == 16);
    CHECK(r2040.constantBuffers[0].variables.size() == 2);
    CHECK(r2040.constantBuffers[0].variables[0].name == "phase");
    CHECK(r2040.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r2040.constantBuffers[0].variables[0].size == 4);
    CHECK(r2040.constantBuffers[0].variables[1].name == "invVague");
    CHECK(r2040.constantBuffers[0].variables[1].startOffset == 4);
    CHECK(r2040.constantBuffers[0].variables[1].size == 4);

    // ps_2043: alphaMulAdd @ offset 0 (size 16), screenMul @ offset 16 (size 8)
    auto ps2043 = readShaderFile("ps_2043.bin");
    CHECK(!ps2043.empty());
    DxbcReflectionData r2043{};
    CHECK(parseDxbcRdef(ps2043.data(), ps2043.size(), r2043));
    CHECK(r2043.constantBuffers.size() == 1);
    CHECK(r2043.constantBuffers[0].size == 32);
    CHECK(r2043.constantBuffers[0].variables.size() == 2);
    CHECK(r2043.constantBuffers[0].variables[0].name == "alphaMulAdd");
    CHECK(r2043.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r2043.constantBuffers[0].variables[1].name == "screenMul");
    CHECK(r2043.constantBuffers[0].variables[1].startOffset == 16);

    // ps_2051: alphaMulAdd @ offset 0 (size 16)
    auto ps2051 = readShaderFile("ps_2051.bin");
    CHECK(!ps2051.empty());
    DxbcReflectionData r2051{};
    CHECK(parseDxbcRdef(ps2051.data(), ps2051.size(), r2051));
    CHECK(r2051.constantBuffers.size() == 1);
    CHECK(r2051.constantBuffers[0].size == 16);
    CHECK(r2051.constantBuffers[0].variables.size() == 1);
    CHECK(r2051.constantBuffers[0].variables[0].name == "alphaMulAdd");
    CHECK(r2051.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r2051.constantBuffers[0].variables[0].size == 16);

    // ps_2072: alphaRef @ offset 0 (size 4)
    auto ps2072 = readShaderFile("ps_2072.bin");
    CHECK(!ps2072.empty());
    DxbcReflectionData r2072{};
    CHECK(parseDxbcRdef(ps2072.data(), ps2072.size(), r2072));
    CHECK(r2072.constantBuffers.size() == 1);
    CHECK(r2072.constantBuffers[0].variables.size() == 1);
    CHECK(r2072.constantBuffers[0].variables[0].name == "alphaRef");
    CHECK(r2072.constantBuffers[0].variables[0].startOffset == 0);
    CHECK(r2072.constantBuffers[0].variables[0].size == 4);
}

static void testVertexShaderCreationTableMapping() {
    // Kiểm tra ánh xạ 12 Vertex Shaders theo bảng unk_140654880 (sub_14054CDE0)
    // vsId = 4 là shader vẽ sprite 2D chính (POSITION + COLOR + TEXCOORD0):
    // Bắt buộc dùng vertex color (isVertexShaderUsingConstColor(4) == false) và có 1 kênh UV
    CHECK(!isVertexShader3D(4));
    CHECK(!isVertexShaderUsingConstColor(4));
    CHECK(getVertexShaderTexCoordCount(4) == 1);

    // vsId = 0, 2, 3, 6, 8, 9 dùng constVtxColor
    CHECK(isVertexShaderUsingConstColor(0));
    CHECK(isVertexShaderUsingConstColor(2));
    CHECK(isVertexShaderUsingConstColor(3));
    CHECK(isVertexShaderUsingConstColor(6));
    CHECK(isVertexShaderUsingConstColor(8));
    CHECK(isVertexShaderUsingConstColor(9));

    // vsId = 1, 4, 5, 7, 10, 11 dùng vertex color aColor
    CHECK(!isVertexShaderUsingConstColor(1));
    CHECK(!isVertexShaderUsingConstColor(4));
    CHECK(!isVertexShaderUsingConstColor(5));
    CHECK(!isVertexShaderUsingConstColor(7));
    CHECK(!isVertexShaderUsingConstColor(10));
    CHECK(!isVertexShaderUsingConstColor(11));

    // Số kênh UV:
    CHECK(getVertexShaderTexCoordCount(0) == 0);
    CHECK(getVertexShaderTexCoordCount(1) == 0);
    CHECK(getVertexShaderTexCoordCount(2) == 1);
    CHECK(getVertexShaderTexCoordCount(3) == 2);
    CHECK(getVertexShaderTexCoordCount(5) == 2);
}

int main() {
    testInvalidAndEdgeCaseInputs();
    testVertexShadersRdef();
    testPixelShadersRdef();
    testVertexShaderCreationTableMapping();

    std::printf("dxbc_reflector tests: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
