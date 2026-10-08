#include "resource_registry.h"

#include <algorithm>
#include <cstring>

#include "bc7_decoder.h"

namespace sgre {

namespace {

constexpr uint32_t kMaxTextureDim = 16384;
constexpr uint32_t kBindRenderTarget = 0x20;
constexpr uint32_t kBindDepthStencil = 0x40;

bool isBc7(uint32_t format) {
    return format == kDxgiBc7Unorm || format == kDxgiBc7UnormSrgb;
}

size_t bc7SourceSpan(uint32_t width, uint32_t height, uint32_t rowPitch) {
    const uint32_t blocksX = (width + kBc7BlockDim - 1) / kBc7BlockDim;
    const uint32_t blocksY = (height + kBc7BlockDim - 1) / kBc7BlockDim;
    const uint32_t pitch = rowPitch != 0 ? rowPitch : bc7DefaultRowPitch(width);
    return static_cast<size_t>(blocksY - 1) * pitch + static_cast<size_t>(blocksX) * kBc7BlockBytes;
}

}  // namespace

uint32_t cpuBytesPerPixel(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case kDxgiR8G8B8A8Unorm:
        case kDxgiB8G8R8A8Unorm:
        case kDxgiD24UnormS8Uint:
        case kDxgiBc7Unorm:
        case kDxgiBc7UnormSrgb:
            return 4;
        case kDxgiA8Unorm:
            return 4;
        default:
            return 0;
    }
}

ResourceRegistry::ResourceRegistry(size_t cpuBudgetBytes)
    : budgetBytes_(cpuBudgetBytes),
      cpuBytes_(0),
      droppedResources_(0),
      totalBuffersCreated_(0),
      totalTexturesCreated_(0) {}

bool ResourceRegistry::reserveBytes(size_t bytes) {
    if (bytes > budgetBytes_ || cpuBytes_ + bytes > budgetBytes_) return false;
    cpuBytes_ += bytes;
    return true;
}

void ResourceRegistry::eraseLocked(std::unordered_map<const void*, Entry>::iterator it) {
    cpuBytes_ -= it->second.data.size();
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

    if (reserveBytes(desc.byteWidth)) {
        entry.data.assign(desc.byteWidth, 0);
        entry.hasData = true;
        if (initData != nullptr) std::memcpy(entry.data.data(), initData, desc.byteWidth);
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

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.count(handle) != 0) return false;

    Entry entry{};
    entry.kind = ResourceKind::Texture2D;
    entry.refCount = 1;
    entry.texture = desc;
    entry.bytesPerPixel = cpuBytesPerPixel(desc.format);
    entry.rowPitch = desc.width * entry.bytesPerPixel;
    entry.hasData = false;

    // Render target / depth không cần bản CPU; format lạ cũng chỉ giữ metadata.
    const bool wantsCpuCopy = entry.bytesPerPixel != 0 &&
                              (desc.bindFlags & (kBindRenderTarget | kBindDepthStencil)) == 0;
    if (wantsCpuCopy) {
        const size_t bytes = static_cast<size_t>(entry.rowPitch) * desc.height;
        if (reserveBytes(bytes)) {
            entry.data.assign(bytes, 0);
            entry.hasData = true;
            if (initData != nullptr) {
                const uint8_t* src = static_cast<const uint8_t*>(initData);
                if (isBc7(desc.format)) {
                    std::vector<uint8_t> decoded;
                    const size_t span = bc7SourceSpan(desc.width, desc.height, initRowPitch);
                    if (decodeBc7ToRgba8(src, span, desc.width, desc.height, initRowPitch, decoded)) {
                        entry.data.swap(decoded);
                    }
                } else if (desc.format == kDxgiA8Unorm) {
                    const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : desc.width;
                    for (uint32_t row = 0; row < desc.height; ++row) {
                        const uint8_t* rowSrc = src + static_cast<size_t>(row) * srcPitch;
                        uint8_t* rowDst = entry.data.data() + static_cast<size_t>(row) * entry.rowPitch;
                        for (uint32_t col = 0; col < desc.width; ++col) {
                            rowDst[col * 4 + 0] = 255;
                            rowDst[col * 4 + 1] = 255;
                            rowDst[col * 4 + 2] = 255;
                            rowDst[col * 4 + 3] = rowSrc[col];
                        }
                    }
                } else if (desc.format == kDxgiB8G8R8A8Unorm) {
                    const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : entry.rowPitch;
                    for (uint32_t row = 0; row < desc.height; ++row) {
                        const uint8_t* rowSrc = src + static_cast<size_t>(row) * srcPitch;
                        uint8_t* rowDst = entry.data.data() + static_cast<size_t>(row) * entry.rowPitch;
                        for (uint32_t col = 0; col < desc.width; ++col) {
                            rowDst[col * 4 + 0] = rowSrc[col * 4 + 2]; // R
                            rowDst[col * 4 + 1] = rowSrc[col * 4 + 1]; // G
                            rowDst[col * 4 + 2] = rowSrc[col * 4 + 0]; // B
                            rowDst[col * 4 + 3] = rowSrc[col * 4 + 3]; // A
                        }
                    }
                } else {
                    const uint32_t srcPitch = initRowPitch != 0 ? initRowPitch : entry.rowPitch;
                    for (uint32_t row = 0; row < desc.height; ++row) {
                        std::memcpy(entry.data.data() + static_cast<size_t>(row) * entry.rowPitch,
                                    src + static_cast<size_t>(row) * srcPitch, entry.rowPitch);
                    }
                }
            }
        } else {
            ++droppedResources_;
        }
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

bool ResourceRegistry::updateBuffer(const void* handle, const UpdateBox* box, const void* data) {
    if (data == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Buffer) return false;

    Entry& entry = it->second;
    if (!entry.hasData) return false;

    uint32_t begin = 0;
    uint32_t end = entry.buffer.byteWidth;
    if (box != nullptr) {
        begin = box->left;
        end = box->right;
    }
    if (begin >= end || end > entry.buffer.byteWidth) return false;

    std::memcpy(entry.data.data() + begin, data, end - begin);
    return true;
}

bool ResourceRegistry::updateTexture(
    const void* handle, const UpdateBox* box, const void* data, uint32_t srcRowPitch) {
    if (data == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || it->second.kind != ResourceKind::Texture2D) return false;

    Entry& entry = it->second;
    if (!entry.hasData) return false;

    const TextureDesc& desc = entry.texture;
    uint32_t left = 0, top = 0, right = desc.width, bottom = desc.height;
    if (box != nullptr) {
        left = box->left;
        top = box->top;
        right = box->right;
        bottom = box->bottom;
    }
    if (left >= right || top >= bottom || right > desc.width || bottom > desc.height) return false;

    const uint8_t* src = static_cast<const uint8_t*>(data);

    if (isBc7(desc.format)) {
        // BC7 chỉ hỗ trợ cập nhật toàn bộ ảnh (vùng con phải căn theo khối 4x4).
        if (left != 0 || top != 0 || right != desc.width || bottom != desc.height) return false;
        std::vector<uint8_t> decoded;
        const size_t span = bc7SourceSpan(desc.width, desc.height, srcRowPitch);
        if (!decodeBc7ToRgba8(src, span, desc.width, desc.height, srcRowPitch, decoded)) return false;
        entry.data.swap(decoded);
        return true;
    }

    if (desc.format == kDxgiA8Unorm) {
        const uint32_t regionPixels = right - left;
        const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : regionPixels;
        for (uint32_t row = top; row < bottom; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row - top) * srcPitch;
            uint8_t* rowDst = entry.data.data() + static_cast<size_t>(row) * entry.rowPitch +
                              static_cast<size_t>(left) * 4;
            for (uint32_t col = 0; col < regionPixels; ++col) {
                rowDst[col * 4 + 0] = 255;
                rowDst[col * 4 + 1] = 255;
                rowDst[col * 4 + 2] = 255;
                rowDst[col * 4 + 3] = rowSrc[col];
            }
        }
        return true;
    }

    if (desc.format == kDxgiB8G8R8A8Unorm) {
        uint32_t effectiveWidth = desc.width;
        const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : (right - left) * 4;
        if (srcPitch > entry.rowPitch && box == nullptr) {
            effectiveWidth = srcPitch / 4;
            entry.texture.width = effectiveWidth;
            entry.rowPitch = srcPitch;
            entry.data.resize(static_cast<size_t>(srcPitch) * desc.height, 0);
        }
        const uint32_t regionPixels = std::min(entry.texture.width - left, srcPitch / 4);
        for (uint32_t row = top; row < bottom; ++row) {
            const uint8_t* rowSrc = src + static_cast<size_t>(row - top) * srcPitch;
            uint8_t* rowDst = entry.data.data() + static_cast<size_t>(row) * entry.rowPitch +
                              static_cast<size_t>(left) * 4;
            for (uint32_t col = 0; col < regionPixels; ++col) {
                rowDst[col * 4 + 0] = rowSrc[col * 4 + 2]; // R
                rowDst[col * 4 + 1] = rowSrc[col * 4 + 1]; // G
                rowDst[col * 4 + 2] = rowSrc[col * 4 + 0]; // B
                rowDst[col * 4 + 3] = rowSrc[col * 4 + 3]; // A
            }
        }
        return true;
    }

    const uint32_t regionRowBytes = (right - left) * entry.bytesPerPixel;
    const uint32_t srcPitch = srcRowPitch != 0 ? srcRowPitch : regionRowBytes;
    for (uint32_t row = top; row < bottom; ++row) {
        std::memcpy(
            entry.data.data() + static_cast<size_t>(row) * entry.rowPitch +
                static_cast<size_t>(left) * entry.bytesPerPixel,
            src + static_cast<size_t>(row - top) * srcPitch, regionRowBytes);
    }
    return true;
}

bool ResourceRegistry::map(const void* handle, MappedView& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || !it->second.hasData) return false;

    Entry& entry = it->second;
    out.data = entry.data.data();
    out.rowPitch = entry.rowPitch;
    out.depthPitch = entry.kind == ResourceKind::Texture2D
                         ? entry.rowPitch * entry.texture.height
                         : entry.buffer.byteWidth;
    return true;
}

bool ResourceRegistry::releaseCpuData(const void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(handle);
    if (it == entries_.end() || !it->second.hasData) return false;

    cpuBytes_ -= it->second.data.size();
    std::vector<uint8_t>().swap(it->second.data);
    it->second.hasData = false;
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
