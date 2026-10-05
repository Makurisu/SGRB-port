#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sgre {

constexpr uint32_t kBc7BlockDim = 4;
constexpr uint32_t kBc7BlockBytes = 16;
constexpr uint32_t kRgba8BytesPerPixel = 4;

// Số byte dữ liệu BC7 nén cho ảnh width x height (làm tròn lên theo khối 4x4).
size_t bc7CompressedSize(uint32_t width, uint32_t height);

// Số byte một hàng khối BC7 (rowPitch mặc định khi D3D không truyền pitch).
uint32_t bc7DefaultRowPitch(uint32_t width);

// Giải nén BC7 sang RGBA8 trên CPU. Hỗ trợ kích thước không chia hết cho 4.
// srcRowPitch = 0 nghĩa là dùng pitch mặc định. Trả về false nếu tham số sai
// hoặc dữ liệu nguồn không đủ lớn.
bool decodeBc7ToRgba8(
    const uint8_t* src,
    size_t srcSize,
    uint32_t width,
    uint32_t height,
    uint32_t srcRowPitch,
    std::vector<uint8_t>& out
);

}  // namespace sgre
