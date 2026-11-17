#include "bc7_decoder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec/bcdec.h"

namespace sgre {

namespace {

constexpr size_t kMaxTexturePixels = 16384ull * 16384ull;
constexpr uint32_t kParallelMinBlockRows = 16;
constexpr uint32_t kWorkChunkRows = 4;

uint32_t blocksAcross(uint32_t pixels) {
    return (pixels + kBc7BlockDim - 1) / kBc7BlockDim;
}

void decodeBlockRowRange(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t blocksX,
    uint32_t pitch,
    size_t dstRowBytes,
    uint32_t startBy,
    uint32_t endBy,
    uint8_t* outData
) {
    uint8_t localTile[kBc7BlockDim * kBc7BlockDim * kRgba8BytesPerPixel];
    constexpr int tilePitch = kBc7BlockDim * kRgba8BytesPerPixel;

    for (uint32_t by = startBy; by < endBy; ++by) {
        const uint8_t* blockRow = src + static_cast<size_t>(by) * pitch;
        const uint32_t py = by * kBc7BlockDim;
        const uint32_t copyH = (height - py) < kBc7BlockDim ? (height - py) : kBc7BlockDim;

        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            bcdec_bc7(blockRow + static_cast<size_t>(bx) * kBc7BlockBytes, localTile, tilePitch);

            const uint32_t px = bx * kBc7BlockDim;
            const uint32_t copyW = (width - px) < kBc7BlockDim ? (width - px) : kBc7BlockDim;

            for (uint32_t row = 0; row < copyH; ++row) {
                uint8_t* dst = outData + (py + row) * dstRowBytes + static_cast<size_t>(px) * kRgba8BytesPerPixel;
                std::memcpy(dst, localTile + row * tilePitch, static_cast<size_t>(copyW) * kRgba8BytesPerPixel);
            }
        }
    }
}

struct Bc7ParallelJob {
    const uint8_t* src = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t blocksX = 0;
    uint32_t blocksY = 0;
    uint32_t pitch = 0;
    size_t dstRowBytes = 0;
    uint8_t* outData = nullptr;
    std::atomic<uint32_t> nextBlockRow{0};
    std::atomic<uint32_t> inFlight{0};
};

// Thread Pool đa tác vụ cố định tránh tạo/huỷ OS thread và tránh fallback đơn luồng khi 2 thread cùng decode
class Bc7ThreadPool {
public:
    static Bc7ThreadPool& instance() {
        static Bc7ThreadPool pool;
        return pool;
    }

    bool tryAcquireAndRun(Bc7ParallelJob& job) {
        if (workers_.empty()) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.inFlight.store(1, std::memory_order_relaxed);
            activeJobs_.push_back(&job);
        }

        workerCv_.notify_all();
        processJobChunks(&job);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            job.inFlight.fetch_sub(1, std::memory_order_acq_rel);
            doneCv_.wait(lock, [&job]() {
                return job.inFlight.load(std::memory_order_acquire) == 0;
            });
            auto it = std::find(activeJobs_.begin(), activeJobs_.end(), &job);
            if (it != activeJobs_.end()) {
                activeJobs_.erase(it);
            }
        }
        return true;
    }

private:
    Bc7ThreadPool() {
        unsigned int hw = std::thread::hardware_concurrency();
        unsigned int workerCount = hw > 1 ? std::min(7u, hw - 1) : 0u;
        workers_.reserve(workerCount);
        for (unsigned int i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~Bc7ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        workerCv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    static void processJobChunks(Bc7ParallelJob* job) {
        while (true) {
            uint32_t startBy = job->nextBlockRow.fetch_add(kWorkChunkRows, std::memory_order_relaxed);
            if (startBy >= job->blocksY) break;
            uint32_t endBy = std::min(job->blocksY, startBy + kWorkChunkRows);
            decodeBlockRowRange(
                job->src, job->width, job->height,
                job->blocksX, job->pitch, job->dstRowBytes,
                startBy, endBy, job->outData
            );
        }
    }

    void workerLoop() {
        while (true) {
            Bc7ParallelJob* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                workerCv_.wait(lock, [this, &job]() {
                    if (stopping_) return true;
                    for (Bc7ParallelJob* candidate : activeJobs_) {
                        if (candidate->nextBlockRow.load(std::memory_order_relaxed) < candidate->blocksY) {
                            job = candidate;
                            return true;
                        }
                    }
                    return false;
                });
                if (stopping_) return;
                if (job != nullptr) {
                    job->inFlight.fetch_add(1, std::memory_order_acq_rel);
                }
            }

            if (job != nullptr) {
                processJobChunks(job);
                if (job->inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    doneCv_.notify_all();
                }
                job = nullptr;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable workerCv_;
    std::condition_variable doneCv_;
    std::vector<std::thread> workers_;
    std::vector<Bc7ParallelJob*> activeJobs_;
    bool stopping_ = false;
};

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

    if (blocksY >= kParallelMinBlockRows) {
        Bc7ParallelJob job;
        job.src = src;
        job.width = width;
        job.height = height;
        job.blocksX = blocksX;
        job.blocksY = blocksY;
        job.pitch = pitch;
        job.dstRowBytes = dstRowBytes;
        job.outData = out.data();
        if (Bc7ThreadPool::instance().tryAcquireAndRun(job)) {
            return true;
        }
    }

    decodeBlockRowRange(src, width, height, blocksX, pitch, dstRowBytes, 0, blocksY, out.data());
    return true;
}

}  // namespace sgre
