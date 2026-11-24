#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sgre {

constexpr size_t kDefaultCpuBudgetBytes = 512ull * 1024 * 1024;
constexpr int32_t kNotTracked = -1;

// Mã định dạng DXGI mà game dùng (xem log CreateTexture2D).
enum DxgiFormat : uint32_t {
    kDxgiR8G8B8A8Unorm = 28,
    kDxgiD24UnormS8Uint = 45,
    kDxgiR8Unorm = 61,
    kDxgiA8Unorm = 65,
    kDxgiB8G8R8A8Unorm = 87,
    kDxgiB8G8R8X8Unorm = 88,
    kDxgiBc7Unorm = 98,
    kDxgiBc7UnormSrgb = 99,
};

enum class ResourceKind { Buffer, Texture2D };

using PixelBufferPtr = std::shared_ptr<std::vector<uint8_t>>;
using ConstPixelBufferPtr = std::shared_ptr<const std::vector<uint8_t>>;

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
    uint32_t revision = 0;
    uint64_t generationId = 0;
    bool isCompressedBc7 = false;
};

struct TextureSnapshot {
    TextureDesc desc{};
    ConstPixelBufferPtr pixels;
    bool isUploadedToGpu = false;
    bool hasGpuContents = false;
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
// Thread-safe. Giữ snapshot bất biến bằng shared_ptr và thu hồi LRU các atlas tĩnh đã lên GPU khi vượt ngân sách.
class ResourceRegistry {
public:
    explicit ResourceRegistry(size_t cpuBudgetBytes = kDefaultCpuBudgetBytes);

    bool registerBuffer(const void* handle, const BufferDesc& desc, const void* initData);
    bool registerTexture(
        const void* handle, const TextureDesc& desc, const void* initData, uint32_t initRowPitch);

    bool contains(const void* handle) const;
    bool getTextureDesc(const void* handle, TextureDesc& out) const;
    bool getBufferDesc(const void* handle, BufferDesc& out) const;

    // Lấy bản chụp metadata + pixel buffer thread-safe (O(1) refcount, không giữ khóa khi upload GLES)
    bool acquireTextureSnapshot(const void* handle, TextureSnapshot& out);

    // Ghi dữ liệu (UpdateSubresource). box = nullptr nghĩa là ghi toàn bộ tài nguyên.
    bool updateBuffer(const void* handle, const UpdateBox* box, const void* data);
    bool updateTexture(
        const void* handle, const UpdateBox* box, const void* data, uint32_t srcRowPitch);

    // Sao chép tài nguyên (CopyResource / CopySubresourceRegion)
    bool copyResource(const void* dstHandle, const void* srcHandle);
    bool copySubresourceRegion(
        const void* dstHandle, uint32_t dstSubresource, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
        const void* srcHandle, uint32_t srcSubresource, const UpdateBox* srcBox);

    // Cấp bộ nhớ CPU cho Map. Trả về false nếu không theo dõi hoặc đã bỏ pixel.
    bool map(const void* handle, MappedView& out);

    // Giải phóng bản CPU sau khi đã upload lên GPU (giữ lại metadata).
    bool releaseCpuData(const void* handle);

    // Đánh dấu texture đã được cập nhật dữ liệu CPU (tăng revision)
    bool touchTexture(const void* handle);

    // Đánh dấu texture đã được Render Thread nạp lên OpenGL ES VRAM thành công
    void markUploadedToGpu(const void* handle);

    // Bảo toàn dữ liệu RGBA8 đọc ngược từ FBO về RAM CPU trước khi hủy EGLContext và xóa cờ đã upload GPU
    bool preserveGpuTextureToCpu(const void* handle, const uint8_t* rgbaData, size_t byteSize);
    void resetAllUploadedFlags();

    // Quản lý trạng thái dữ liệu nằm trên GPU (sinh ra từ RenderTarget hoặc CopyResource GPU-to-GPU)
    bool isGpuOwned(const void* handle) const;
    bool setGpuOwned(const void* handle, bool gpuOwned);

    // Đếm tham chiếu COM. Trả về số tham chiếu còn lại, hoặc kNotTracked.
    int32_t addRef(const void* handle);
    int32_t release(const void* handle);

    void setBptcEnabled(bool enabled) { bptcEnabled_.store(enabled, std::memory_order_relaxed); }
    bool isBptcEnabled() const { return bptcEnabled_.load(std::memory_order_relaxed); }

    // Giải nén BC7 sang RGBA8 nếu driver GLES không thể dùng bản nén BPTC trực tiếp hoặc khi cần truy cập CPU
    bool ensureTextureDecompressed(const void* handle);

    ResourceStats stats() const;

private:
    struct Entry {
        ResourceKind kind;
        uint32_t refCount;
        BufferDesc buffer;
        TextureDesc texture;
        PixelBufferPtr data;
        uint32_t rowPitch;
        uint32_t bytesPerPixel;
        uint64_t lastUsedSeq = 0;
        bool hasData;
        bool isUploadedToGpu;
        bool hasGpuContents;
    };

    bool reserveBytes(size_t bytes);
    void evictCpuBuffersLocked(size_t requiredBytes);
    bool isEvictableImmutableLocked(const Entry& entry) const;
    void ensureUniqueBufferLocked(Entry& entry);
    bool ensureEntryDecompressedLocked(Entry& entry);
    void eraseLocked(std::unordered_map<const void*, Entry>::iterator it);

    mutable std::mutex mutex_;
    std::unordered_map<const void*, Entry> entries_;
    std::atomic<bool> bptcEnabled_{false};
    size_t budgetBytes_;
    uint64_t cpuBytes_;
    uint64_t accessSeq_ = 0;
    uint32_t droppedResources_;
    uint64_t totalBuffersCreated_;
    uint64_t totalTexturesCreated_;
};

}  // namespace sgre
