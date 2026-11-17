// Test native chạy trên host (g++), không phụ thuộc Android.
// Build: g++ -std=c++17 -Wall -I app/src/main/cpp tests/native/test_resource_registry.cpp \
//        app/src/main/cpp/resource_registry.cpp app/src/main/cpp/bc7_decoder.cpp -o test_registry
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bc7_decoder.h"
#include "resource_registry.h"

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

static BufferDesc makeBufferDesc(uint32_t bytes) {
    BufferDesc d{};
    d.byteWidth = bytes;
    return d;
}

static TextureDesc makeTextureDesc(uint32_t w, uint32_t h, uint32_t format, uint32_t bind = 0x8) {
    TextureDesc d{};
    d.width = w;
    d.height = h;
    d.mipLevels = 1;
    d.arraySize = 1;
    d.format = format;
    d.sampleCount = 1;
    d.bindFlags = bind;
    return d;
}

// Khối BC7 mode 6 với mọi bit dữ liệu = 1: mọi pixel phải là (255,255,255,255).
static void fillOpaqueWhiteBc7Block(uint8_t* block) {
    std::memset(block, 0xFF, kBc7BlockBytes);
    block[0] = 0xC0;
}

static void testBufferLifecycle() {
    ResourceRegistry reg;
    int handle = 0;
    uint8_t init[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(reg.registerBuffer(&handle, makeBufferDesc(8), init));
    CHECK(reg.contains(&handle));

    MappedView view{};
    CHECK(reg.map(&handle, view));
    CHECK(view.data != nullptr && view.data[0] == 1 && view.data[7] == 8);
    CHECK(view.rowPitch == 8);

    uint8_t patch[2] = {0xAA, 0xBB};
    UpdateBox box{2, 0, 0, 4, 1, 1};
    CHECK(reg.updateBuffer(&handle, &box, patch));
    CHECK(view.data[2] == 0xAA && view.data[3] == 0xBB && view.data[4] == 5);

    CHECK(reg.release(&handle) == 0);
    CHECK(!reg.contains(&handle));
    CHECK(reg.stats().cpuBytes == 0);
}

static void testBufferInvalidInputs() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(!reg.registerBuffer(nullptr, makeBufferDesc(8), nullptr));
    CHECK(!reg.registerBuffer(&handle, makeBufferDesc(0), nullptr));
    CHECK(reg.registerBuffer(&handle, makeBufferDesc(8), nullptr));
    CHECK(!reg.registerBuffer(&handle, makeBufferDesc(8), nullptr));  // trùng handle

    uint8_t data[16] = {};
    UpdateBox outOfRange{4, 0, 0, 12, 1, 1};
    UpdateBox empty{4, 0, 0, 4, 1, 1};
    CHECK(!reg.updateBuffer(&handle, &outOfRange, data));
    CHECK(!reg.updateBuffer(&handle, &empty, data));
    CHECK(!reg.updateBuffer(&handle, nullptr, nullptr));
    int unknown = 0;
    CHECK(!reg.updateBuffer(&unknown, nullptr, data));
    MappedView view{};
    CHECK(!reg.map(&unknown, view));
}

static void testRefCounting() {
    ResourceRegistry reg;
    int handle = 0;
    int unknown = 0;
    CHECK(reg.registerBuffer(&handle, makeBufferDesc(4), nullptr));
    CHECK(reg.addRef(&handle) == 2);
    CHECK(reg.release(&handle) == 1);
    CHECK(reg.contains(&handle));
    CHECK(reg.release(&handle) == 0);
    CHECK(!reg.contains(&handle));
    CHECK(reg.addRef(&unknown) == kNotTracked);
    CHECK(reg.release(&unknown) == kNotTracked);
}

static void testTextureRawUploadWithPitch() {
    ResourceRegistry reg;
    int handle = 0;
    // 2x2 A8, pitch nguồn 4 byte (có padding).
    uint8_t init[8] = {1, 2, 0xEE, 0xEE, 3, 4, 0xEE, 0xEE};
    CHECK(reg.registerTexture(&handle, makeTextureDesc(2, 2, kDxgiA8Unorm), init, 4));
    MappedView view{};
    CHECK(reg.map(&handle, view));
    CHECK(view.rowPitch == 2);
    CHECK(view.data[0] == 1 && view.data[1] == 2 && view.data[2] == 3 && view.data[3] == 4);

    TextureDesc desc{};
    CHECK(reg.getTextureDesc(&handle, desc));
    CHECK(desc.width == 2 && desc.height == 2 && desc.format == kDxgiA8Unorm);
}

static void testTextureRegionUpdate() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(reg.registerTexture(&handle, makeTextureDesc(4, 4, kDxgiA8Unorm), nullptr, 0));
    uint8_t patch[4] = {9, 9, 9, 9};
    UpdateBox box{1, 1, 0, 3, 3, 1};  // vùng 2x2 tại (1,1)
    CHECK(reg.updateTexture(&handle, &box, patch, 2));
    MappedView view{};
    CHECK(reg.map(&handle, view));
    CHECK(view.data[0] == 0);
    CHECK(view.data[1 * 4 + 1] == 9 && view.data[2 * 4 + 2] == 9);
    CHECK(view.data[3 * 4 + 3] == 0);

    UpdateBox bad{0, 0, 0, 5, 4, 1};  // vượt chiều rộng
    CHECK(!reg.updateTexture(&handle, &bad, patch, 5));
}

static void testRenderTargetKeepsNoCpuCopy() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(reg.registerTexture(&handle, makeTextureDesc(1920, 1080, kDxgiR8G8B8A8Unorm, 0x28), nullptr, 0));
    CHECK(reg.contains(&handle));
    MappedView view{};
    CHECK(!reg.map(&handle, view));
    CHECK(reg.stats().cpuBytes == 0);
}

static void testUnsupportedFormatKeepsMetadata() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(reg.registerTexture(&handle, makeTextureDesc(8, 8, 9999), nullptr, 0));
    TextureDesc desc{};
    CHECK(reg.getTextureDesc(&handle, desc));
    MappedView view{};
    CHECK(!reg.map(&handle, view));
}

static void testTextureInvalidInputs() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(!reg.registerTexture(nullptr, makeTextureDesc(4, 4, kDxgiA8Unorm), nullptr, 0));
    CHECK(!reg.registerTexture(&handle, makeTextureDesc(0, 4, kDxgiA8Unorm), nullptr, 0));
    CHECK(!reg.registerTexture(&handle, makeTextureDesc(4, 0, kDxgiA8Unorm), nullptr, 0));
    CHECK(!reg.registerTexture(&handle, makeTextureDesc(20000, 4, kDxgiA8Unorm), nullptr, 0));
}

static void testBudgetDropsPixelsButKeepsMetadata() {
    ResourceRegistry reg(1024);
    int small = 0;
    int big = 0;
    CHECK(reg.registerBuffer(&small, makeBufferDesc(512), nullptr));
    CHECK(reg.registerTexture(&big, makeTextureDesc(64, 64, kDxgiR8G8B8A8Unorm), nullptr, 0));
    CHECK(reg.contains(&big));
    MappedView view{};
    CHECK(!reg.map(&big, view));
    ResourceStats s = reg.stats();
    CHECK(s.droppedResources == 1);
    CHECK(s.cpuBytes == 512);
    // Giải phóng buffer nhỏ rồi texture nhỏ hơn ngân sách phải được giữ pixel.
    CHECK(reg.release(&small) == 0);
    int fits = 0;
    CHECK(reg.registerTexture(&fits, makeTextureDesc(16, 16, kDxgiR8G8B8A8Unorm), nullptr, 0));
    CHECK(reg.map(&fits, view));
}

static void testReleaseCpuData() {
    ResourceRegistry reg;
    int handle = 0;
    CHECK(reg.registerBuffer(&handle, makeBufferDesc(256), nullptr));
    CHECK(reg.stats().cpuBytes == 256);
    CHECK(reg.releaseCpuData(&handle));
    CHECK(reg.stats().cpuBytes == 0);
    CHECK(reg.contains(&handle));
    CHECK(!reg.releaseCpuData(&handle));
}

static void testBc7Sizes() {
    CHECK(bc7CompressedSize(4, 4) == 16);
    CHECK(bc7CompressedSize(5, 5) == 64);  // làm tròn lên 2x2 khối
    CHECK(bc7DefaultRowPitch(8) == 32);
    CHECK(bc7CompressedSize(256, 256) == 64 * 64 * 16);
}

static void testBc7DecodeOpaqueWhite() {
    uint8_t block[kBc7BlockBytes];
    fillOpaqueWhiteBc7Block(block);
    std::vector<uint8_t> out;
    CHECK(decodeBc7ToRgba8(block, sizeof(block), 4, 4, 0, out));
    CHECK(out.size() == 4 * 4 * 4);
    bool allWhite = true;
    for (uint8_t v : out) allWhite = allWhite && v == 0xFF;
    CHECK(allWhite);
}

static void testBc7DecodeClipsNonMultipleOf4() {
    // 6x5 -> 2x2 khối; mọi khối trắng đục.
    std::vector<uint8_t> src(4 * kBc7BlockBytes);
    for (int i = 0; i < 4; ++i) fillOpaqueWhiteBc7Block(src.data() + i * kBc7BlockBytes);
    std::vector<uint8_t> out;
    CHECK(decodeBc7ToRgba8(src.data(), src.size(), 6, 5, 0, out));
    CHECK(out.size() == 6 * 5 * 4);
    bool allWhite = true;
    for (uint8_t v : out) allWhite = allWhite && v == 0xFF;
    CHECK(allWhite);
}

static void testBc7DecodeRejectsBadInput() {
    uint8_t block[kBc7BlockBytes];
    fillOpaqueWhiteBc7Block(block);
    std::vector<uint8_t> out;
    CHECK(!decodeBc7ToRgba8(nullptr, 16, 4, 4, 0, out));
    CHECK(!decodeBc7ToRgba8(block, sizeof(block), 0, 4, 0, out));
    CHECK(!decodeBc7ToRgba8(block, sizeof(block), 4, 0, 0, out));
    CHECK(!decodeBc7ToRgba8(block, 8, 4, 4, 0, out));            // nguồn thiếu
    CHECK(!decodeBc7ToRgba8(block, sizeof(block), 8, 4, 0, out));  // cần 2 khối
    CHECK(!decodeBc7ToRgba8(block, sizeof(block), 4, 4, 8, out));  // pitch < 1 hàng khối
    CHECK(!decodeBc7ToRgba8(block, sizeof(block), 20000, 20000, 0, out));
}

static void testBc7TextureThroughRegistry() {
    ResourceRegistry reg;
    int handle = 0;
    uint8_t block[kBc7BlockBytes];
    fillOpaqueWhiteBc7Block(block);
    CHECK(reg.registerTexture(&handle, makeTextureDesc(4, 4, kDxgiBc7Unorm), block, 16));
    MappedView view{};
    CHECK(reg.map(&handle, view));
    CHECK(view.rowPitch == 16);  // 4 pixel * 4 byte (đã giải nén RGBA8)
    CHECK(view.data[0] == 0xFF && view.data[63] == 0xFF);
    CHECK(reg.stats().cpuBytes == 64);

    UpdateBox partial{0, 0, 0, 2, 2, 1};
    CHECK(!reg.updateTexture(&handle, &partial, block, 16));  // BC7 không cho cập nhật vùng con
    CHECK(reg.updateTexture(&handle, nullptr, block, 16));
}

static void testStatsCounters() {
    ResourceRegistry reg;
    int b = 0, t = 0;
    CHECK(reg.registerBuffer(&b, makeBufferDesc(16), nullptr));
    CHECK(reg.registerTexture(&t, makeTextureDesc(2, 2, kDxgiA8Unorm), nullptr, 0));
    ResourceStats s = reg.stats();
    CHECK(s.liveBuffers == 1 && s.liveTextures == 1);
    CHECK(s.totalBuffersCreated == 1 && s.totalTexturesCreated == 1);
    reg.release(&b);
    s = reg.stats();
    CHECK(s.liveBuffers == 0 && s.totalBuffersCreated == 1);
}

static void testBufferGetDesc() {
    ResourceRegistry reg;
    int b = 0;
    BufferDesc inDesc = makeBufferDesc(128);
    inDesc.bindFlags = 0x1;
    inDesc.structureByteStride = 20;
    CHECK(reg.registerBuffer(&b, inDesc, nullptr));

    BufferDesc outDesc{};
    CHECK(reg.getBufferDesc(&b, outDesc));
    CHECK(outDesc.byteWidth == 128);
    CHECK(outDesc.bindFlags == 0x1);
    CHECK(outDesc.structureByteStride == 20);

    int unknown = 1;
    BufferDesc unkDesc{};
    CHECK(!reg.getBufferDesc(&unknown, unkDesc));
}

int main() {
    testBufferLifecycle();
    testBufferInvalidInputs();
    testBufferGetDesc();
    testRefCounting();
    testTextureRawUploadWithPitch();
    testTextureRegionUpdate();
    testRenderTargetKeepsNoCpuCopy();
    testUnsupportedFormatKeepsMetadata();
    testTextureInvalidInputs();
    testBudgetDropsPixelsButKeepsMetadata();
    testReleaseCpuData();
    testBc7Sizes();
    testBc7DecodeOpaqueWhite();
    testBc7DecodeClipsNonMultipleOf4();
    testBc7DecodeRejectsBadInput();
    testBc7TextureThroughRegistry();
    testStatsCounters();

    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
