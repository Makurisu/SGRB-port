#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sgre {

constexpr size_t kDefaultCpuBudgetBytes = 768ull * 1024 * 1024;
constexpr int32_t kNotTracked = -1;

// Mã định dạng DXGI mà game dùng (xem log CreateTexture2D).
enum DxgiFormat : uint32_t {
    kDxgiR8G8B8A8Unorm = 28,
    kDxgiD24UnormS8Uint = 45,
    kDxgiA8Unorm = 65,
    kDxgiB8G8R8A8Unorm = 87,
    kDxgiBc7Unorm = 98,
    kDxgiBc7UnormSrgb = 99,
};

enum class ResourceKind { Buffer, Texture2D };

struct BufferDesc {
    uint32_t byteWidth;
    uint32_t usage;
    uint32_t bindFlags;
    uint32_t cpuAccessFlags;
    uint32_t miscFlags;
    uint32_t structureByteStride;
};

struct TextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t format;
    uint32_t sampleCount;
    uint32_t sampleQuality;
    uint32_t usage;
    uint32_t bindFlags;
    uint32_t cpuAccessFlags;
    uint32_t miscFlags;
};

// Vùng ghi con (D3D11_BOX): left/right theo byte với buffer, theo pixel với texture.
struct UpdateBox {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
};

struct ResourceStats {
    uint32_t liveBuffers;
    uint32_t liveTextures;
    uint32_t droppedResources;
    uint64_t cpuBytes;
    uint64_t totalBuffersCreated;
    uint64_t totalTexturesCreated;
};

struct MappedView {
    uint8_t* data;
    uint32_t rowPitch;
    uint32_t depthPitch;
};

// Số byte mỗi pixel của bản lưu trên CPU (BC7 được giải nén sang RGBA8). 0 = không hỗ trợ.
uint32_t cpuBytesPerPixel(uint32_t dxgiFormat);

// Kho tài nguyên D3D11 phía CPU, khoá theo con trỏ COM mà game giữ.
// Thread-safe. Vượt ngân sách bộ nhớ thì chỉ giữ metadata (không giữ pixel).
class ResourceRegistry {
public:
    explicit ResourceRegistry(size_t cpuBudgetBytes = kDefaultCpuBudgetBytes);

    bool registerBuffer(const void* handle, const BufferDesc& desc, const void* initData);
    bool registerTexture(
        const void* handle, const TextureDesc& desc, const void* initData, uint32_t initRowPitch);

    bool contains(const void* handle) const;
    bool getTextureDesc(const void* handle, TextureDesc& out) const;

    // Ghi dữ liệu (UpdateSubresource). box = nullptr nghĩa là ghi toàn bộ tài nguyên.
    bool updateBuffer(const void* handle, const UpdateBox* box, const void* data);
    bool updateTexture(
        const void* handle, const UpdateBox* box, const void* data, uint32_t srcRowPitch);

    // Cấp bộ nhớ CPU cho Map. Trả về false nếu không theo dõi hoặc đã bỏ pixel.
    bool map(const void* handle, MappedView& out);

    // Giải phóng bản CPU sau khi đã upload lên GPU (giữ lại metadata).
    bool releaseCpuData(const void* handle);

    // Đếm tham chiếu COM. Trả về số tham chiếu còn lại, hoặc kNotTracked.
    int32_t addRef(const void* handle);
    int32_t release(const void* handle);

    ResourceStats stats() const;

private:
    struct Entry {
        ResourceKind kind;
        uint32_t refCount;
        BufferDesc buffer;
        TextureDesc texture;
        std::vector<uint8_t> data;
        uint32_t rowPitch;
        uint32_t bytesPerPixel;
        bool hasData;
    };

    bool reserveBytes(size_t bytes);
    void eraseLocked(std::unordered_map<const void*, Entry>::iterator it);

    mutable std::mutex mutex_;
    std::unordered_map<const void*, Entry> entries_;
    size_t budgetBytes_;
    uint64_t cpuBytes_;
    uint32_t droppedResources_;
    uint64_t totalBuffersCreated_;
    uint64_t totalTexturesCreated_;
};

}  // namespace sgre
