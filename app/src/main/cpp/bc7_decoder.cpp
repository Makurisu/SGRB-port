#include "bc7_decoder.h"

#include <cstring>

#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec/bcdec.h"

namespace sgre {

namespace {

constexpr size_t kMaxTexturePixels = 16384ull * 16384ull;

uint32_t blocksAcross(uint32_t pixels) {
    return (pixels + kBc7BlockDim - 1) / kBc7BlockDim;
}

}  // namespace

size_t bc7CompressedSize(uint32_t width, uint32_t height) {
    return static_cast<size_t>(blocksAcross(width)) * blocksAcross(height) * kBc7BlockBytes;
}

uint32_t bc7DefaultRowPitch(uint32_t width) {
    return blocksAcross(width) * kBc7BlockBytes;
}

bool decodeBc7ToRgba8(
    const uint8_t* src,
    size_t srcSize,
    uint32_t width,
    uint32_t height,
    uint32_t srcRowPitch,
    std::vector<uint8_t>& out
) {
    if (src == nullptr || width == 0 || height == 0) return false;
    if (static_cast<size_t>(width) * height > kMaxTexturePixels) return false;

    const uint32_t blocksX = blocksAcross(width);
    const uint32_t blocksY = blocksAcross(height);
    const uint32_t pitch = srcRowPitch != 0 ? srcRowPitch : bc7DefaultRowPitch(width);
    if (pitch < blocksX * kBc7BlockBytes) return false;

    // Hàng khối cuối chỉ cần đủ blocksX khối, không cần đủ pitch.
    const size_t required = static_cast<size_t>(blocksY - 1) * pitch + blocksX * kBc7BlockBytes;
    if (srcSize < required) return false;

    const size_t dstRowBytes = static_cast<size_t>(width) * kRgba8BytesPerPixel;
    out.assign(dstRowBytes * height, 0);

    uint8_t tile[kBc7BlockDim * kBc7BlockDim * kRgba8BytesPerPixel];
    const int tilePitch = kBc7BlockDim * kRgba8BytesPerPixel;

    for (uint32_t by = 0; by < blocksY; ++by) {
        const uint8_t* blockRow = src + static_cast<size_t>(by) * pitch;
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            bcdec_bc7(blockRow + static_cast<size_t>(bx) * kBc7BlockBytes, tile, tilePitch);

            const uint32_t px = bx * kBc7BlockDim;
            const uint32_t py = by * kBc7BlockDim;
            const uint32_t copyW = (width - px) < kBc7BlockDim ? (width - px) : kBc7BlockDim;
            const uint32_t copyH = (height - py) < kBc7BlockDim ? (height - py) : kBc7BlockDim;

            for (uint32_t row = 0; row < copyH; ++row) {
                uint8_t* dst = out.data() + (py + row) * dstRowBytes + static_cast<size_t>(px) * kRgba8BytesPerPixel;
                std::memcpy(dst, tile + row * tilePitch, static_cast<size_t>(copyW) * kRgba8BytesPerPixel);
            }
        }
    }
    return true;
}

}  // namespace sgre
