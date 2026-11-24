#include "resource_registry.h"

#include <algorithm>
#include <cstring>

#include "bc7_decoder.h"

namespace sgre {

namespace {

constexpr uint32_t kMaxTextureDim = 16384;
constexpr uint32_t kBindShaderResource = 0x8;
constexpr uint32_t kBindRenderTarget = 0x20;
constexpr uint32_t kBindDepthStencil = 0x40;
constexpr uint32_t kUsageImmutable = 1;
constexpr uint32_t kUsageStaging = 3;

bool isBc7(uint32_t format) {
    return format == kDxgiBc7Unorm || format == kDxgiBc7UnormSrgb;
}

size_t bc7SourceSpan(uint32_t width, uint32_t height, uint32_t rowPitch) {
    const uint32_t blocksX = (width + kBc7BlockDim - 1) / kBc7BlockDim;
    const uint32_t blocksY = (height + kBc7BlockDim - 1) / kBc7BlockDim;
    const uint32_t pitch = rowPitch != 0 ? rowPitch : bc7DefaultRowPitch(width);
    return static_cast<size_t>(blocksY - 1) * pitch + static_cast<size_t>(blocksX) * kBc7BlockBytes;
}

bool canStoreCompressedBc7(const TextureDesc& desc, const void* initData, uint32_t initRowPitch, bool bptcEnabled) {
    if (!bptcEnabled || initData == nullptr) return false;
    if (desc.format != kDxgiBc7Unorm) return false;
    if ((desc.bindFlags & kBindRenderTarget) != 0) return false;
    if (desc.usage == kUsageStaging || desc.cpuAccessFlags != 0) return false;
    if ((desc.width % kBc7BlockDim) != 0 || (desc.height % kBc7BlockDim) != 0) return false;
    return initRowPitch == 0 || initRowPitch == bc7DefaultRowPitch(desc.width);
}

// Chuyển đổi/giải nén dữ liệu pixel bên ngoài mutex_ để không block Main Thread và Pre-Uploader
bool prepareTexturePixelsLockFree(
    const TextureDesc& desc,
    uint32_t rowPitch,
    const void* initData,
    uint32_t initRowPitch,
    bool bptcEnabled,
    PixelBufferPtr& outPixels,
    bool& outIsCompressedBc7,
    bool& outDecodedOk
) {
    outIsCompressedBc7 = false;
    outDecodedOk = (initData != nullptr);
    const uint8_t* src = static_cast<const uint8_t*>(initData);

    if (canStoreCompressedBc7(desc, initData, initRowPitch, bptcEnabled)) {
        const size_t span = bc7SourceSpan(desc.width, desc.height, initRowPitch);
        outPixels = std::make_shared<std::vector<uint8_t>>(span);
        std::memcpy(outPixels->data(), src, span);
        outIsCompressedBc7 = true;
        return true;
    }

    const size_t rgbaBytes = static_cast<size_t>(rowPitch) * desc.height;
    if (initData != nullptr && isBc7(desc.format)) {
        outPixels = std::make_shared<std::vector<uint8_t>>();
        const size_t span = bc7SourceSpan(desc.width, desc.height, initRowPitch);
        if (!decodeBc7ToRgba8(src, span, desc.width, desc.height, initRowPitch, *outPixels)) {
            outPixels->assign(rgbaBytes, 0);
            outDecodedOk = false;
        }
        return true;
    }

    outPixels = std::make_shared<std::vector<uint8_t>>(rgbaBytes, 0);
    if (initData == nullptr) {
        return true;
    }

    if (desc.format == kDxgiA8Unorm || desc.format == kDxgiR8Unorm) {
        const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : desc.width;
        for (uint32_t row = 0; row < desc.height; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row) * srcPitch;
            uint8_t* rowDst = outPixels->data() + static_cast<size_t>(row) * rowPitch;
            for (uint32_t col = 0; col < desc.width; ++col) {
                rowDst[col * 4 + 0] = rowSrc[col];
                rowDst[col * 4 + 1] = rowSrc[col];
                rowDst[col * 4 + 2] = rowSrc[col];
                rowDst[col * 4 + 3] = rowSrc[col];
            }
        }
    } else if (desc.format == kDxgiB8G8R8A8Unorm) {
        const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : rowPitch;
        for (uint32_t row = 0; row < desc.height; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row) * srcPitch;
            uint8_t* rowDst = outPixels->data() + static_cast<size_t>(row) * rowPitch;
            for (uint32_t col = 0; col < desc.width; ++col) {
                rowDst[col * 4 + 0] = rowSrc[col * 4 + 2];
                rowDst[col * 4 + 1] = rowSrc[col * 4 + 1];
                rowDst[col * 4 + 2] = rowSrc[col * 4 + 0];
                rowDst[col * 4 + 3] = rowSrc[col * 4 + 3];
            }
        }
    } else if (desc.format == kDxgiB8G8R8X8Unorm) {
        const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : rowPitch;
        for (uint32_t row = 0; row < desc.height; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row) * srcPitch;
            uint8_t* rowDst = outPixels->data() + static_cast<size_t>(row) * rowPitch;
            for (uint32_t col = 0; col < desc.width; ++col) {
                rowDst[col * 4 + 0] = rowSrc[col * 4 + 2];
                rowDst[col * 4 + 1] = rowSrc[col * 4 + 1];
                rowDst[col * 4 + 2] = rowSrc[col * 4 + 0];
                rowDst[col * 4 + 3] = 0xFF;
            }
        }
    } else {
        const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : rowPitch;
        for (uint32_t row = 0; row < desc.height; ++row) {
            std::memcpy(outPixels->data() + static_cast<size_t>(row) * rowPitch,
                        src + static_cast<size_t>(row) * srcPitch, rowPitch);
        }
    }
    return true;
}

}  // namespace

uint32_t cpuBytesPerPixel(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case kDxgiR8G8B8A8Unorm:
        case kDxgiB8G8R8A8Unorm:
        case kDxgiB8G8R8X8Unorm:
        case kDxgiD24UnormS8Uint:
        case kDxgiBc7Unorm:
        case kDxgiBc7UnormSrgb:
            return 4;
        case kDxgiR8Unorm:
        case kDxgiA8Unorm:
            return 4;
        default:
            return 0;
    }
}

ResourceRegistry::ResourceRegistry(size_t cpuBudgetBytes)
    : budgetBytes_(cpuBudgetBytes),
      cpuBytes_(0),
      accessSeq_(0),
      droppedResources_(0),
      totalBuffersCreated_(0),
      totalTexturesCreated_(0) {}

bool ResourceRegistry::isEvictableImmutableLocked(const Entry& entry) const {
    if (entry.kind != ResourceKind::Texture2D || !entry.isUploadedToGpu || !entry.hasData) {
        return false;
    }
    if (!entry.data || entry.data->empty()) return false;
    if ((entry.texture.bindFlags & kBindRenderTarget) != 0) return false;
    if (entry.texture.usage == kUsageStaging || entry.texture.cpuAccessFlags != 0) return false;
    // Chỉ thu hồi texture bất biến đã upload lên GPU (BC7 chưa từng sửa hoặc USAGE_IMMUTABLE);
    // giữ nguyên dynamic font atlas, mask A8_UNORM và staging/thumbnail.
    return (isBc7(entry.texture.format) && entry.texture.revision <= 1) ||
           (entry.texture.usage == kUsageImmutable);
}

void ResourceRegistry::evictCpuBuffersLocked(size_t requiredBytes) {
    if (cpuBytes_ + requiredBytes <= budgetBytes_) return;

    struct Candidate {
        uint64_t seq;
        std::unordered_map<const void*, Entry>::iterator it;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(64);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (isEvictableImmutableLocked(it->second)) {
            candidates.push_back({it->second.lastUsedSeq, it});
        }
    }
    if (candidates.empty()) return;

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.seq < b.seq;
    });

    for (const auto& c : candidates) {
        if (cpuBytes_ + requiredBytes <= budgetBytes_) break;
        Entry& entry = c.it->second;
        if (entry.data) {
            cpuBytes_ -= entry.data->size();
            entry.data.reset();
        }
        entry.hasData = false;
    }
}

void ResourceRegistry::ensureUniqueBufferLocked(Entry& entry) {
    if (entry.data && entry.data.use_count() > 1) {
        entry.data = std::make_shared<std::vector<uint8_t>>(*entry.data);
    }
}

bool ResourceRegistry::ensureEntryDecompressedLocked(Entry& entry) {
    if (entry.kind != ResourceKind::Texture2D || !entry.texture.isCompressedBc7) {
        return true;
    }
    if (!entry.data || entry.data->empty()) {
        entry.texture.isCompressedBc7 = false;
        return false;
    }
    auto decoded = std::make_shared<std::vector<uint8_t>>();
    if (!decodeBc7ToRgba8(entry.data->data(), entry.data->size(),
                          entry.texture.width, entry.texture.height, 0, *decoded)) {
        return false;
    }
    const size_t oldBytes = entry.data->size();
    const size_t newBytes = decoded->size();
    if (newBytes > oldBytes) {
        reserveBytes(newBytes - oldBytes);
    } else {
        cpuBytes_ -= (oldBytes - newBytes);
    }
    entry.data = std::move(decoded);
    entry.texture.isCompressedBc7 = false;
    entry.texture.revision++;
    return true;
}

bool ResourceRegistry::reserveBytes(size_t bytes) {
    if (cpuBytes_ + bytes > budgetBytes_) {
        evictCpuBuffersLocked(bytes);
    }
    // Cơ chế Soft Budget an toàn: sau khi thu hồi LRU các atlas tĩnh đã lên GPU,
    // vẫn luôn cho phép cấp phát nếu còn texture động cần giữ lại.
    cpuBytes_ += bytes;
    return true;
}

void ResourceRegistry::eraseLocked(std::unordered_map<const void*, Entry>::iterator it) {
    if (it->second.data) {
        cpuBytes_ -= it->second.data->size();
        it->second.data.reset();
    }
    entries_.erase(it);
}

bool ResourceRegistry::registerBuffer(
    const void* handle, const BufferDesc& desc, const void* initData) {
    if (handle == nullptr || desc.byteWidth == 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.count(handle) != 0) return false;

    Entry entry{};
    entry.kind = ResourceKind::Buffer;
    entry.refCount = 1;
    entry.buffer = desc;
    entry.rowPitch = desc.byteWidth;
    entry.bytesPerPixel = 1;
    entry.lastUsedSeq = ++accessSeq_;

    if (reserveBytes(desc.byteWidth)) {
        entry.data = std::make_shared<std::vector<uint8_t>>(desc.byteWidth, 0);
        entry.hasData = true;
        if (initData != nullptr) std::memcpy(entry.data->data(), initData, desc.byteWidth);
    } else {
        entry.hasData = false;
        ++droppedResources_;
    }

    entries_.emplace(handle, std::move(entry));
    ++totalBuffersCreated_;
    return true;
}

bool ResourceRegistry::registerTexture(
    const void* handle, const TextureDesc& desc, const void* initData, uint32_t initRowPitch) {
    if (handle == nullptr || desc.width == 0 || desc.height == 0) return false;
    if (desc.width > kMaxTextureDim || desc.height > kMaxTextureDim) return false;

    const uint32_t bpp = cpuBytesPerPixel(desc.format);
    const uint32_t rowPitch = desc.width * bpp;

    // Render target / depth GPU thuần túy không cần bản CPU để tránh lãng phí RAM và tránh nhầm lẫn buffer 0 là ảnh.
    // Ngoại lệ: Offscreen Render Target dùng làm Mask (Format 65 A8_UNORM, Format 61 R8_UNORM)
    // hoặc Staging Thumbnail Save (480x270, 284x160, 568x320) bắt buộc phải cấp phát bộ nhớ CPU để M2 Engine đọc/ghi.
    const bool isOffscreenMask = (desc.format == kDxgiA8Unorm || desc.format == kDxgiR8Unorm);
    const bool isThumbnail = ((desc.bindFlags & kBindRenderTarget) == 0) &&
                             ((desc.width == 480 && desc.height == 270) ||
                              (desc.width == 284 && desc.height == 160) ||
                              (desc.width == 568 && desc.height == 320));
    const bool isRenderTarget = (desc.bindFlags & kBindRenderTarget) != 0;
    const bool isDepthStencil = (desc.bindFlags & kBindDepthStencil) != 0;
    const bool isGpuOnlyRenderTarget = isRenderTarget && !isOffscreenMask && !isThumbnail &&
                                       (desc.cpuAccessFlags == 0) && (desc.usage == 0 /* DEFAULT */);

    const bool wantsCpuCopy = bpp != 0 && !isDepthStencil && !isGpuOnlyRenderTarget;

    PixelBufferPtr preparedPixels;
    bool isCompressedBc7 = false;
    bool decodedOk = true;
    if (wantsCpuCopy) {
        prepareTexturePixelsLockFree(
            desc, rowPitch, initData, initRowPitch, bptcEnabled_.load(std::memory_order_relaxed),
            preparedPixels, isCompressedBc7, decodedOk
        );
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.count(handle) != 0) return false;

    Entry entry{};
    entry.kind = ResourceKind::Texture2D;
    entry.refCount = 1;
    entry.texture = desc;
    entry.texture.revision = 1;
    entry.texture.isCompressedBc7 = isCompressedBc7;
    entry.bytesPerPixel = bpp;
    entry.rowPitch = rowPitch;
    entry.lastUsedSeq = ++accessSeq_;
    entry.hasData = wantsCpuCopy && (initData != nullptr || isOffscreenMask || isThumbnail);
    if (!decodedOk && isBc7(desc.format)) {
        entry.hasData = true;
    }
    entry.isUploadedToGpu = false;
    entry.hasGpuContents = false;

    if (preparedPixels && !preparedPixels->empty()) {
        reserveBytes(preparedPixels->size());
        entry.data = std::move(preparedPixels);
    }

    entries_.emplace(handle, std::move(entry));
    ++totalTexturesCreated_;
    return true;
}

bool ResourceRegistry::contains(const void* handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.count(handle) != 0;
}

bool ResourceRegistry::getTextureDesc(const void* handle, TextureDesc& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;
    out = it->second.texture;
    return true;
}

bool ResourceRegistry::getBufferDesc(const void* handle, BufferDesc& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Buffer) return false;
    out = it->second.buffer;
    return true;
}

bool ResourceRegistry::acquireTextureSnapshot(const void* handle, TextureSnapshot& out) {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;
    it->second.lastUsedSeq = ++accessSeq_;
    out.desc = it->second.texture;
    out.pixels = it->second.data;
    out.isUploadedToGpu = it->second.isUploadedToGpu;
    out.hasGpuContents = it->second.hasGpuContents;
    return true;
}

bool ResourceRegistry::updateBuffer(const void* handle, const UpdateBox* box, const void* data) {
    if (data == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Buffer) return false;

    Entry& entry = it->second;
    if (!entry.hasData || !entry.data) {
        if (entry.buffer.byteWidth > 0) {
            entry.data = std::make_shared<std::vector<uint8_t>>(entry.buffer.byteWidth, 0);
            entry.hasData = true;
            cpuBytes_ += entry.buffer.byteWidth;
        } else {
            return false;
        }
    } else {
        ensureUniqueBufferLocked(entry);
    }

    uint32_t begin = 0;
    uint32_t end = entry.buffer.byteWidth;
    if (box != nullptr) {
        begin = box->left;
        end = box->right;
    }
    if (begin >= end || end > entry.buffer.byteWidth) return false;

    std::memcpy(entry.data->data() + begin, data, end - begin);
    return true;
}

bool ResourceRegistry::updateTexture(
    const void* handle, const UpdateBox* box, const void* data, uint32_t srcRowPitch) {
    if (data == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;

    Entry& entry = it->second;
    const TextureDesc& desc = entry.texture;
    uint32_t left = 0, top = 0, right = desc.width, bottom = desc.height;
    if (box != nullptr) {
        left = box->left;
        top = box->top;
        right = box->right;
        bottom = box->bottom;
    }
    if (left >= right || top >= bottom || right > desc.width || bottom > desc.height) return false;

    const bool isFullReplace = (left == 0 && top == 0 && right == desc.width && bottom == desc.height);
    if (!isFullReplace && entry.texture.isCompressedBc7) {
        if (!ensureEntryDecompressedLocked(entry)) return false;
    }

    if (!entry.data || entry.data->empty()) {
        const size_t bytes = static_cast<size_t>(entry.rowPitch) * entry.texture.height;
        if (bytes > 0) {
            reserveBytes(bytes);
            entry.data = std::make_shared<std::vector<uint8_t>>(bytes, 0);
        }
    } else {
        ensureUniqueBufferLocked(entry);
    }
    entry.hasData = true;
    entry.hasGpuContents = false;
    entry.lastUsedSeq = ++accessSeq_;

    const uint8_t* src = static_cast<const uint8_t*>(data);

    if (isBc7(desc.format)) {
        const uint32_t subW = right - left;
        const uint32_t subH = bottom - top;
        std::vector<uint8_t> decoded;
        const size_t span = bc7SourceSpan(subW, subH, srcRowPitch);
        if (!decodeBc7ToRgba8(src, span, subW, subH, srcRowPitch, decoded)) return false;

        if (isFullReplace) {
            const size_t oldBytes = entry.data ? entry.data->size() : 0;
            const size_t newBytes = decoded.size();
            if (newBytes > oldBytes) reserveBytes(newBytes - oldBytes);
            else cpuBytes_ -= (oldBytes - newBytes);
            entry.data->swap(decoded);
            entry.texture.isCompressedBc7 = false;
        } else {
            const uint32_t subRowBytes = subW * 4;
            for (uint32_t r = 0; r < subH; ++r) {
                const uint8_t* pSubRow = decoded.data() + static_cast<size_t>(r) * subRowBytes;
                uint8_t* pDstRow = entry.data->data() + static_cast<size_t>(top + r) * entry.rowPitch + static_cast<size_t>(left) * 4;
                std::memcpy(pDstRow, pSubRow, subRowBytes);
            }
        }
        entry.texture.revision++;
        return true;
    }

    if (desc.format == kDxgiA8Unorm || desc.format == kDxgiR8Unorm) {
        const uint32_t regionPixels = right - left;
        const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : regionPixels;
        for (uint32_t row = top; row < bottom; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row - top) * srcPitch;
            uint8_t* rowDst = entry.data->data() + static_cast<size_t>(row) * entry.rowPitch +
                              static_cast<size_t>(left) * 4;
            for (uint32_t col = 0; col < regionPixels; ++col) {
                rowDst[col * 4 + 0] = rowSrc[col];
                rowDst[col * 4 + 1] = rowSrc[col];
                rowDst[col * 4 + 2] = rowSrc[col];
                rowDst[col * 4 + 3] = rowSrc[col];
            }
        }
        entry.texture.revision++;
        return true;
    }

    if (desc.format == kDxgiB8G8R8A8Unorm) {
        const uint32_t boxPixels = right - left;

        // Phát hiện glyph font 1-byte Alpha (A8) được M2 Engine nạp vào Texture Atlas B8G8R8A8 qua UpdateSubresource.
        // Khi srcRowPitch < boxPixels * 4 và >= boxPixels: nguồn là 1-byte alpha per pixel!
        const bool isSingleByteAlpha = (srcRowPitch != 0 && srcRowPitch < boxPixels * 4 && srcRowPitch >= boxPixels);
        if (isSingleByteAlpha) {
            const uint32_t srcPitch = srcRowPitch;
            for (uint32_t row = top; row < bottom; ++row) {
                const uint8_t* rowSrc = src + static_cast<size_t>(row - top) * srcPitch;
                uint32_t* rowDstDwords = reinterpret_cast<uint32_t*>(entry.data->data() + static_cast<size_t>(row) * entry.rowPitch +
                                                                     static_cast<size_t>(left) * 4);
                for (uint32_t col = 0; col < boxPixels; ++col) {
                    uint8_t a = rowSrc[col];
                    // Unpack 1-byte alpha thành pixel màu trắng RGBA chuẩn (A=a, R=255, G=255, B=255)
                    rowDstDwords[col] = (static_cast<uint32_t>(a) << 24) | 0x00FFFFFFu;
                }
            }
            entry.texture.revision++;
            return true;
        }

        // Nếu nguồn và đích cùng một buffer (in-place update sau khi Map), dữ liệu đã ở trong buffer,
        // không swizzle lần hai để tránh hoán đổi ngược kênh R <-> B
        if (src == entry.data->data()) {
            entry.texture.revision++;
            return true;
        }

        const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : boxPixels * 4;
        const uint32_t maxPixelsFromSrc = srcPitch / 4;
        const uint32_t regionPixels = std::min(boxPixels, maxPixelsFromSrc);

        // Tối ưu hóa khối nhớ phẳng liên tục (Fullscreen Video Frame 1920x1080) qua 32-bit dword swizzle
        auto swizzleBgraToRgba = [](uint32_t px) -> uint32_t {
            uint32_t a = (px >> 24) & 0xFF;
            uint32_t r = (px >> 16) & 0xFF;
            uint32_t g = (px >> 8) & 0xFF;
            uint32_t b = px & 0xFF;
            // Tự động khôi phục Alpha từ độ sáng RGB nếu texture không có Alpha (như font glyph GDI vẽ A=0 nhưng có nét chữ)
            if (a == 0 && (r > 0 || g > 0 || b > 0)) {
                a = std::max({r, g, b});
            }
            return (a << 24) | (b << 16) | (g << 8) | r;
        };

        if (left == 0 && top == 0 && right == desc.width && bottom == desc.height &&
            srcPitch == entry.rowPitch && regionPixels == desc.width) {
            const size_t totalPixels = static_cast<size_t>(desc.width) * desc.height;
            const uint32_t* srcDwords = reinterpret_cast<const uint32_t*>(src);
            uint32_t* dstDwords = reinterpret_cast<uint32_t*>(entry.data->data());
            for (size_t i = 0; i < totalPixels; ++i) {
                dstDwords[i] = swizzleBgraToRgba(srcDwords[i]);
            }
            entry.texture.revision++;
            return true;
        }

        for (uint32_t row = top; row < bottom; ++row) {
            const uint32_t* rowSrcDwords = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(row - top) * srcPitch);
            uint32_t* rowDstDwords = reinterpret_cast<uint32_t*>(entry.data->data() + static_cast<size_t>(row) * entry.rowPitch +
                                                                 static_cast<size_t>(left) * 4);
            for (uint32_t col = 0; col < regionPixels; ++col) {
                rowDstDwords[col] = swizzleBgraToRgba(rowSrcDwords[col]);
            }
        }
        entry.texture.revision++;
        return true;
    }

    if (desc.format == kDxgiB8G8R8X8Unorm) {
        const uint32_t boxPixels = right - left;
        const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : boxPixels * 4;
        const uint32_t maxPixelsFromSrc = srcPitch / 4;
        const uint32_t regionPixels = std::min(boxPixels, maxPixelsFromSrc);

        if (left == 0 && top == 0 && right == desc.width && bottom == desc.height &&
            srcPitch == entry.rowPitch && regionPixels == desc.width) {
            const size_t totalPixels = static_cast<size_t>(desc.width) * desc.height;
            const uint32_t* srcDwords = reinterpret_cast<const uint32_t*>(src);
            uint32_t* dstDwords = reinterpret_cast<uint32_t*>(entry.data->data());
            for (size_t i = 0; i < totalPixels; ++i) {
                uint32_t px = srcDwords[i];
                dstDwords[i] = 0xFF000000u | (px & 0x0000FF00u) | ((px & 0x00FF0000u) >> 16) | ((px & 0x000000FFu) << 16);
            }
            entry.texture.revision++;
            return true;
        }

        for (uint32_t row = top; row < bottom; ++row) {
            const uint32_t* rowSrcDwords = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(row - top) * srcPitch);
            uint32_t* rowDstDwords = reinterpret_cast<uint32_t*>(entry.data->data() + static_cast<size_t>(row) * entry.rowPitch +
                                                                 static_cast<size_t>(left) * 4);
            for (uint32_t col = 0; col < regionPixels; ++col) {
                uint32_t px = rowSrcDwords[col];
                rowDstDwords[col] = 0xFF000000u | (px & 0x0000FF00u) | ((px & 0x00FF0000u) >> 16) | ((px & 0x000000FFu) << 16);
            }
        }
        entry.texture.revision++;
        return true;
    }

    const uint32_t regionRowBytes = (right - left) * entry.bytesPerPixel;
    const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : regionRowBytes;
    for (uint32_t row = top; row < bottom; ++row) {
        std::memcpy(
            entry.data->data() + static_cast<size_t>(row) * entry.rowPitch +
                static_cast<size_t>(left) * entry.bytesPerPixel,
            src + static_cast<size_t>(row - top) * srcPitch, regionRowBytes);
    }
    entry.texture.revision++;
    return true;
}

bool ResourceRegistry::copyResource(const void* dstHandle, const void* srcHandle) {
    if (dstHandle == nullptr || srcHandle == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto itDst = entries_.find(dstHandle);
    auto itSrc = entries_.find(srcHandle);
    if (itDst == entries_.end() || itSrc == entries_.end()) return false;

    Entry& dst = itDst->second;
    Entry& src = itSrc->second;
    src.lastUsedSeq = ++accessSeq_;
    dst.lastUsedSeq = ++accessSeq_;

    if (src.hasData && src.data) {
        if (src.texture.isCompressedBc7 &&
            (dst.kind != ResourceKind::Texture2D || dst.texture.format != src.texture.format)) {
            if (!ensureEntryDecompressedLocked(src)) return false;
        }
        if (!dst.data || dst.data->size() != src.data->size()) {
            size_t oldBytes = dst.data ? dst.data->size() : 0;
            size_t newBytes = src.data->size();
            if (newBytes > oldBytes) reserveBytes(newBytes - oldBytes);
            else cpuBytes_ -= (oldBytes - newBytes);
            dst.data = std::make_shared<std::vector<uint8_t>>(newBytes, 0);
        } else {
            ensureUniqueBufferLocked(dst);
        }
        dst.hasData = true;
        size_t copyBytes = std::min(dst.data->size(), src.data->size());
        std::memcpy(dst.data->data(), src.data->data(), copyBytes);
        dst.hasGpuContents = false;
        if (dst.kind == ResourceKind::Texture2D) {
            dst.texture.isCompressedBc7 = src.texture.isCompressedBc7;
            dst.texture.revision++;
        }
        return true;
    }
    return false;
}

bool ResourceRegistry::copySubresourceRegion(
    const void* dstHandle, uint32_t dstSubresource, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
    const void* srcHandle, uint32_t srcSubresource, const UpdateBox* srcBox) {
    if (dstHandle == nullptr || srcHandle == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto itDst = entries_.find(dstHandle);
    auto itSrc = entries_.find(srcHandle);
    if (itDst == entries_.end() || itSrc == entries_.end()) return false;

    Entry& dst = itDst->second;
    Entry& src = itSrc->second;
    if (!src.hasData || !src.data) return false;
    if (!ensureEntryDecompressedLocked(src) || !ensureEntryDecompressedLocked(dst)) return false;

    src.lastUsedSeq = ++accessSeq_;
    dst.lastUsedSeq = ++accessSeq_;

    if (!dst.data || dst.data->empty()) {
        const size_t bytes = static_cast<size_t>(dst.rowPitch) * (dst.kind == ResourceKind::Texture2D ? dst.texture.height : 1);
        if (bytes > 0 && reserveBytes(bytes)) {
            dst.data = std::make_shared<std::vector<uint8_t>>(bytes, 0);
            dst.hasData = true;
        } else {
            return false;
        }
    } else {
        ensureUniqueBufferLocked(dst);
    }

    if (dst.kind == ResourceKind::Texture2D && src.kind == ResourceKind::Texture2D) {
        uint32_t sLeft = 0, sTop = 0, sRight = src.texture.width, sBottom = src.texture.height;
        if (srcBox != nullptr) {
            sLeft = srcBox->left;
            sTop = srcBox->top;
            sRight = std::min(srcBox->right, src.texture.width);
            sBottom = std::min(srcBox->bottom, src.texture.height);
        }
        if (sLeft >= sRight || sTop >= sBottom) return false;

        uint32_t copyWidth = sRight - sLeft;
        uint32_t copyHeight = sBottom - sTop;
        if (dstX + copyWidth > dst.texture.width || dstY + copyHeight > dst.texture.height) return false;

        if (src.bytesPerPixel == 1 && dst.bytesPerPixel == 4) {
            for (uint32_t row = 0; row < copyHeight; ++row) {
                const uint8_t* pSrcRow = src.data->data() + static_cast<size_t>(sTop + row) * src.rowPitch + static_cast<size_t>(sLeft);
                uint32_t* pDstRow = reinterpret_cast<uint32_t*>(dst.data->data() + static_cast<size_t>(dstY + row) * dst.rowPitch + static_cast<size_t>(dstX) * 4);
                for (uint32_t col = 0; col < copyWidth; ++col) {
                    uint8_t a = pSrcRow[col];
                    pDstRow[col] = (static_cast<uint32_t>(a) << 24) | 0x00FFFFFFu;
                }
            }
        } else {
            uint32_t bpp = std::min(dst.bytesPerPixel, src.bytesPerPixel);
            if (bpp == 0) bpp = 4;
            for (uint32_t row = 0; row < copyHeight; ++row) {
                const uint8_t* pSrcRow = src.data->data() + static_cast<size_t>(sTop + row) * src.rowPitch + static_cast<size_t>(sLeft) * src.bytesPerPixel;
                uint8_t* pDstRow = dst.data->data() + static_cast<size_t>(dstY + row) * dst.rowPitch + static_cast<size_t>(dstX) * dst.bytesPerPixel;
                std::memcpy(pDstRow, pSrcRow, static_cast<size_t>(copyWidth) * bpp);
            }
        }
        dst.hasGpuContents = false;
        dst.texture.revision++;
        return true;
    } else if (dst.kind == ResourceKind::Buffer && src.kind == ResourceKind::Buffer) {
        uint32_t sLeft = 0, sRight = src.buffer.byteWidth;
        if (srcBox != nullptr) {
            sLeft = srcBox->left;
            sRight = std::min(srcBox->right, src.buffer.byteWidth);
        }
        if (sLeft >= sRight) return false;
        uint32_t copyBytes = sRight - sLeft;
        if (dstX + copyBytes > dst.buffer.byteWidth) return false;
        std::memcpy(dst.data->data() + dstX, src.data->data() + sLeft, copyBytes);
        return true;
    }
    return false;
}

bool ResourceRegistry::map(const void* handle, MappedView& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || !it->second.data || it->second.data->empty()) return false;

    Entry& entry = it->second;
    if (!ensureEntryDecompressedLocked(entry)) return false;
    ensureUniqueBufferLocked(entry);
    entry.lastUsedSeq = ++accessSeq_;
    out.data = entry.data->data();
    out.rowPitch = entry.rowPitch;
    out.depthPitch = entry.kind == ResourceKind::Texture2D
                         ? entry.rowPitch * entry.texture.height
                         : entry.buffer.byteWidth;
    return true;
}

bool ResourceRegistry::ensureTextureDecompressed(const void* handle) {
    if (handle == nullptr) return false;
    ConstPixelBufferPtr compressed;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(handle);
        if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;
        if (!it->second.texture.isCompressedBc7) return true;
        if (!it->second.data || it->second.data->empty()) {
            it->second.texture.isCompressedBc7 = false;
            return false;
        }
        compressed = it->second.data;
        w = it->second.texture.width;
        h = it->second.texture.height;
    }

    auto decoded = std::make_shared<std::vector<uint8_t>>();
    if (!decodeBc7ToRgba8(compressed->data(), compressed->size(), w, h, 0, *decoded)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;
    Entry& entry = it->second;
    if (!entry.texture.isCompressedBc7 || entry.data != compressed) {
        return true;
    }
    const size_t oldBytes = entry.data ? entry.data->size() : 0;
    const size_t newBytes = decoded->size();
    if (newBytes > oldBytes) {
        reserveBytes(newBytes - oldBytes);
    } else {
        cpuBytes_ -= (oldBytes - newBytes);
    }
    entry.data = std::move(decoded);
    entry.texture.isCompressedBc7 = false;
    entry.texture.revision++;
    return true;
}

bool ResourceRegistry::releaseCpuData(const void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || !it->second.hasData) return false;

    if (it->second.data) {
        cpuBytes_ -= it->second.data->size();
        it->second.data.reset();
    }
    it->second.hasData = false;
    return true;
}

bool ResourceRegistry::touchTexture(const void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;
    it->second.hasData = (it->second.data && !it->second.data->empty());
    it->second.lastUsedSeq = ++accessSeq_;
    it->second.texture.revision++;
    return true;
}

void ResourceRegistry::markUploadedToGpu(const void* handle) {
    if (handle == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it != entries_.end() && it->second.kind == ResourceKind::Texture2D) {
        it->second.isUploadedToGpu = true;
    }
}

bool ResourceRegistry::preserveGpuTextureToCpu(const void* handle, const uint8_t* rgbaData, size_t byteSize) {
    if (handle == nullptr || rgbaData == nullptr || byteSize == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;

    Entry& entry = it->second;
    const size_t expectedBytes = static_cast<size_t>(entry.texture.width) * entry.texture.height * 4ull;
    if (expectedBytes == 0 || byteSize < expectedBytes) return false;

    auto preserved = std::make_shared<std::vector<uint8_t>>(rgbaData, rgbaData + expectedBytes);
    const size_t oldBytes = (entry.hasData && entry.data) ? entry.data->size() : 0;
    if (expectedBytes > oldBytes) {
        reserveBytes(expectedBytes - oldBytes);
    } else if (oldBytes > expectedBytes) {
        cpuBytes_ -= (oldBytes - expectedBytes);
    }

    entry.data = std::move(preserved);
    entry.rowPitch = entry.texture.width * 4u;
    entry.bytesPerPixel = 4u;
    entry.hasData = true;
    entry.isUploadedToGpu = false;
    entry.hasGpuContents = false;
    entry.texture.isCompressedBc7 = false;
    entry.texture.revision++;
    entry.lastUsedSeq = ++accessSeq_;
    return true;
}

void ResourceRegistry::resetAllUploadedFlags() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : entries_) {
        if (kv.second.kind == ResourceKind::Texture2D) {
            kv.second.isUploadedToGpu = false;
        }
    }
}

bool ResourceRegistry::isGpuOwned(const void* handle) const {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    return it != entries_.end() && it->second.hasGpuContents;
}

bool ResourceRegistry::setGpuOwned(const void* handle, bool gpuOwned) {
    if (handle == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end()) return false;
    it->second.hasGpuContents = gpuOwned;
    return true;
}

int32_t ResourceRegistry::addRef(const void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end()) return kNotTracked;
    return static_cast<int32_t>(++it->second.refCount);
}

int32_t ResourceRegistry::release(const void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end()) return kNotTracked;
    if (it->second.refCount > 0) --it->second.refCount;
    const int32_t remaining = static_cast<int32_t>(it->second.refCount);
    if (remaining == 0) eraseLocked(it);
    return remaining;
}

ResourceStats ResourceRegistry::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ResourceStats s{};
    for (const auto& kv : entries_) {
        if (kv.second.kind == ResourceKind::Buffer) ++s.liveBuffers;
        else ++s.liveTextures;
    }
    s.droppedResources = droppedResources_;
    s.cpuBytes = cpuBytes_;
    s.totalBuffersCreated = totalBuffersCreated_;
    s.totalTexturesCreated = totalTexturesCreated_;
    return s;
}

}  // namespace sgre
