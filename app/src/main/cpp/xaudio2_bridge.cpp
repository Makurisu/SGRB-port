#include "xaudio2_bridge.h"
#include "wma_decoder.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define LOG_TAG "SGRE_XAudio2Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

extern "C" bool Win32Bridge_SetupCurrentThreadTEB();

namespace {

constexpr uint32_t kMasterSampleRate = 48000u;
constexpr uint32_t kMasterChannels = 2u;
constexpr size_t kMixBlockFrames = 256u;
constexpr size_t kMixBlockSamples = kMixBlockFrames * kMasterChannels;

static int32_t WINAPI stub_XAudioSuccess() { return 0; }

struct XAUDIO2_VOICE_DETAILS {
    uint32_t CreationFlags;
    uint32_t ActiveFlags;
    uint32_t InputChannels;
    uint32_t InputSampleRate;
};

// Cấu trúc XAUDIO2_BUFFER chuẩn x64 MSVC ABI (48 bytes, pContext tại offset 40 = 0x28)
struct XAUDIO2_BUFFER_64 {
    uint32_t Flags;
    uint32_t AudioBytes;
    const uint8_t* pAudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    uint32_t pad;
    void* pContext;
};

struct XAUDIO2_VOICE_STATE_64 {
    void* pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};

// Bố cục vtable của IXAudio2VoiceCallback trong game (IDA 0x14038E720, 0x14038E670)
using FnOnBufferStart = void (WINAPI *)(void* thisPtr, void* pBufferContext);
using FnOnBufferEnd = void (WINAPI *)(void* thisPtr, void* pBufferContext);

// ============================================================================
// Bộ giải mã Microsoft ADPCM (MS-ADPCM) chuẩn RFC/WAVE format tag 2
// ============================================================================
static const int32_t MSADPCM_COEF1[7] = { 256, 512, 0, 192, 240, 460, 392 };
static const int32_t MSADPCM_COEF2[7] = { 0, -256, 0, 64, 0, -208, -232 };

static const int32_t MSADPCM_ADAPTATION_TABLE[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};

static inline int16_t clampPcm16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return static_cast<int16_t>(val);
}

static inline int16_t msadpcm_decode_nibble(
    uint8_t nibble,
    int32_t coef1, int32_t coef2,
    int32_t& delta,
    int32_t& samp1, int32_t& samp2
) {
    int32_t signedNibble = (nibble & 0x08) ? (static_cast<int32_t>(nibble) - 16) : static_cast<int32_t>(nibble);
    int32_t pred = (samp1 * coef1 + samp2 * coef2) / 256;
    int32_t newSample = pred + signedNibble * delta;
    int16_t clamped = clampPcm16(newSample);

    samp2 = samp1;
    samp1 = clamped;

    delta = (delta * MSADPCM_ADAPTATION_TABLE[nibble]) / 256;
    if (delta < 16) delta = 16;

    return clamped;
}

static bool decodeMsAdpcm(
    const uint8_t* src, uint32_t srcBytes,
    uint32_t channels, uint32_t blockAlign,
    const int32_t* customCoef1, const int32_t* customCoef2, uint32_t numCoefs,
    std::vector<int16_t>& outPcm
) {
    if (!src || srcBytes == 0 || channels == 0 || blockAlign == 0) return false;
    outPcm.clear();

    const int32_t* coef1 = customCoef1 ? customCoef1 : MSADPCM_COEF1;
    const int32_t* coef2 = customCoef2 ? customCoef2 : MSADPCM_COEF2;
    uint32_t maxCoefs = (customCoef1 && numCoefs > 0) ? numCoefs : 7;

    uint32_t offset = 0;
    while (offset + blockAlign <= srcBytes) {
        const uint8_t* block = src + offset;
        offset += blockAlign;

        if (channels == 2) {
            if (blockAlign < 14) break;
            uint8_t pred0 = block[0];
            uint8_t pred1 = block[1];
            if (pred0 >= maxCoefs || pred1 >= maxCoefs) continue;

            int32_t c1_0 = coef1[pred0], c2_0 = coef2[pred0];
            int32_t c1_1 = coef1[pred1], c2_1 = coef2[pred1];

            int32_t delta0 = *reinterpret_cast<const int16_t*>(block + 2);
            int32_t delta1 = *reinterpret_cast<const int16_t*>(block + 4);
            int32_t samp1_0 = *reinterpret_cast<const int16_t*>(block + 6);
            int32_t samp1_1 = *reinterpret_cast<const int16_t*>(block + 8);
            int32_t samp2_0 = *reinterpret_cast<const int16_t*>(block + 10);
            int32_t samp2_1 = *reinterpret_cast<const int16_t*>(block + 12);

            outPcm.push_back(static_cast<int16_t>(samp2_0));
            outPcm.push_back(static_cast<int16_t>(samp2_1));
            outPcm.push_back(static_cast<int16_t>(samp1_0));
            outPcm.push_back(static_cast<int16_t>(samp1_1));

            for (uint32_t b = 14; b < blockAlign; ++b) {
                uint8_t byteVal = block[b];
                uint8_t nL = (byteVal >> 4) & 0x0F;
                uint8_t nR = byteVal & 0x0F;
                outPcm.push_back(msadpcm_decode_nibble(nL, c1_0, c2_0, delta0, samp1_0, samp2_0));
                outPcm.push_back(msadpcm_decode_nibble(nR, c1_1, c2_1, delta1, samp1_1, samp2_1));
            }
        } else if (channels == 1) {
            if (blockAlign < 7) break;
            uint8_t pred = block[0];
            if (pred >= maxCoefs) continue;

            int32_t c1 = coef1[pred], c2 = coef2[pred];
            int32_t delta = *reinterpret_cast<const int16_t*>(block + 1);
            int32_t samp1 = *reinterpret_cast<const int16_t*>(block + 3);
            int32_t samp2 = *reinterpret_cast<const int16_t*>(block + 5);

            outPcm.push_back(static_cast<int16_t>(samp2));
            outPcm.push_back(static_cast<int16_t>(samp1));

            for (uint32_t b = 7; b < blockAlign; ++b) {
                uint8_t byteVal = block[b];
                uint8_t n0 = (byteVal >> 4) & 0x0F;
                uint8_t n1 = byteVal & 0x0F;
                outPcm.push_back(msadpcm_decode_nibble(n0, c1, c2, delta, samp1, samp2));
                outPcm.push_back(msadpcm_decode_nibble(n1, c1, c2, delta, samp1, samp2));
            }
        }
    }
    return !outPcm.empty();
}

static void resampleLinearStereo(
    const int16_t* inStereo, size_t inFrames, uint32_t inRate,
    uint32_t outRate, std::vector<int16_t>& outStereo
) {
    if (!inStereo || inFrames == 0 || inRate == 0 || outRate == 0) return;
    if (inRate == outRate) {
        outStereo.assign(inStereo, inStereo + inFrames * 2u);
        return;
    }
    size_t outFrames = std::max<size_t>(1u, (inFrames * outRate) / inRate);
    outStereo.resize(outFrames * 2u);
    double ratio = static_cast<double>(inRate) / static_cast<double>(outRate);

    for (size_t i = 0; i < outFrames; ++i) {
        double srcPos = static_cast<double>(i) * ratio;
        size_t idx0 = static_cast<size_t>(srcPos);
        size_t idx1 = std::min(idx0 + 1u, inFrames - 1u);
        double frac = srcPos - static_cast<double>(idx0);

        for (uint32_t ch = 0; ch < 2u; ++ch) {
            double s0 = inStereo[idx0 * 2u + ch];
            double s1 = inStereo[idx1 * 2u + ch];
            double s = s0 + frac * (s1 - s0);
            outStereo[i * 2u + ch] = clampPcm16(static_cast<int32_t>(std::lround(s)));
        }
    }
}

struct PreparedAudioChunk {
    void* pContext = nullptr;
    std::vector<int16_t> stereoSamples; // Luôn chuẩn hóa sẵn sang Stereo 2 kênh ở tần số mixer
    size_t totalMixFrames = 0;
    size_t cursorFrame = 0;
    uint64_t sourceFrames = 0;
    bool started = false;
};

class SourceVoiceInstance;

struct PendingVoiceCallback {
    SourceVoiceInstance* voice = nullptr;
    void* callbackPtr = nullptr;
    void* pContext = nullptr;
    bool isBufferStart = false;
};

// ============================================================================
// XAudio2MasterMixer: Bộ trộn âm thanh trung tâm duy nhất cho toàn tiến trình
// Sử dụng 1 AAudioStream chia sẻ và 1 luồng trộn phần mềm thay vì mở 32+ luồng phần cứng.
// ============================================================================
class XAudio2MasterMixer {
public:
    static XAudio2MasterMixer& instance() {
        static XAudio2MasterMixer sMixer;
        return sMixer;
    }

    uint32_t outputSampleRate() const {
        return actualSampleRate_.load(std::memory_order_relaxed);
    }

    void setMasterVolume(float vol) {
        masterVolume_.store(std::clamp(vol, 0.0f, 4.0f), std::memory_order_relaxed);
    }

    float getMasterVolume() const {
        return masterVolume_.load(std::memory_order_relaxed);
    }

    void setLifecyclePaused(bool paused) {
        lifecyclePaused_.store(paused, std::memory_order_relaxed);
        cv_.notify_all();
    }

    void registerVoice(SourceVoiceInstance* voice) {
        ensureStarted();
        {
            std::lock_guard<std::mutex> lock(voicesMutex_);
            voices_.push_back(voice);
        }
        cv_.notify_all();
    }

    void unregisterVoice(SourceVoiceInstance* voice) {
        std::unique_lock<std::mutex> lock(voicesMutex_);
        // Đợi nếu luồng mixer đang thực thi callback cho đúng voice này để tránh use-after-free
        callbackDoneCv_.wait(lock, [&]() { return activeCallbackVoice_ != voice; });
        voices_.erase(std::remove(voices_.begin(), voices_.end(), voice), voices_.end());
    }

    void notifyWorkAvailable() {
        cv_.notify_one();
    }

private:
    XAudio2MasterMixer() = default;

    ~XAudio2MasterMixer() {
        running_.store(false);
        cv_.notify_all();
        if (mixerThread_.joinable()) {
            mixerThread_.join();
        }
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }

    void ensureStarted() {
        std::lock_guard<std::mutex> lock(initMutex_);
        if (started_.load(std::memory_order_acquire)) return;

        openSharedStream();
        running_.store(true, std::memory_order_release);
        mixerThread_ = std::thread(&XAudio2MasterMixer::mixerLoop, this);
        started_.store(true, std::memory_order_release);
    }

    void openSharedStream() {
        if (stream_) {
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
            return;
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(kMasterSampleRate));
        AAudioStreamBuilder_setChannelCount(builder, static_cast<int32_t>(kMasterChannels));
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

        aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);

        if (res == AAUDIO_OK && stream_) {
            int32_t bursts = AAudioStream_getFramesPerBurst(stream_);
            int32_t cap = AAudioStream_getBufferCapacityInFrames(stream_);
            if (bursts > 0 && cap > 0) {
                int32_t targetBuf = std::min(cap, std::max(bursts * 4, static_cast<int32_t>(kMixBlockFrames * 4)));
                AAudioStream_setBufferSizeInFrames(stream_, targetBuf);
            }
            uint32_t hwRate = static_cast<uint32_t>(AAudioStream_getSampleRate(stream_));
            if (hwRate > 0) {
                actualSampleRate_.store(hwRate, std::memory_order_relaxed);
            }
            LOGI("XAudio2MasterMixer: Đã mở AAudioStream trung tâm (%u Hz, 2 kênh, burst=%d, cap=%d)",
                 actualSampleRate_.load(), bursts, cap);
        } else {
            LOGW("XAudio2MasterMixer: Không mở được AAudioStream (%d), chuyển sang đồng hồ ảo", res);
            stream_ = nullptr;
        }
    }

    void mixerLoop();

    std::mutex initMutex_;
    std::mutex voicesMutex_;
    std::condition_variable cv_;
    std::condition_variable callbackDoneCv_;
    std::vector<SourceVoiceInstance*> voices_;
    SourceVoiceInstance* activeCallbackVoice_ = nullptr;
    AAudioStream* stream_ = nullptr;
    std::thread mixerThread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> lifecyclePaused_{false};
    std::atomic<uint32_t> actualSampleRate_{kMasterSampleRate};
    std::atomic<float> masterVolume_{1.0f};
};

class SourceVoiceInstance {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc khớp COM interface ABI

    SourceVoiceInstance(
        void* pCallback, uint32_t avgBytesPerSec, uint32_t sampleRate = 48000,
        uint32_t channels = 2, uint32_t bitsPerSample = 16,
        uint16_t formatTag = 1, uint16_t blockAlign = 4, const void* pSourceFormat = nullptr
    )
        : callback_(pCallback),
          avgBytesPerSec_(avgBytesPerSec > 0 ? avgBytesPerSec : 192000),
          sampleRate_(sampleRate > 0 ? sampleRate : 48000),
          channels_(channels > 0 ? channels : 2),
          bitsPerSample_(bitsPerSample > 0 ? bitsPerSample : 16),
          formatTag_(formatTag),
          blockAlign_(blockAlign > 0 ? blockAlign : 4) {
        initVtable();
        lpVtbl = vtable_;

        if (formatTag_ == 2 && pSourceFormat) {
            const auto* rawFmt = reinterpret_cast<const uint8_t*>(pSourceFormat);
            uint16_t cbSize = *reinterpret_cast<const uint16_t*>(rawFmt + 16);
            if (cbSize >= 4) {
                samplesPerBlock_ = *reinterpret_cast<const uint16_t*>(rawFmt + 18);
                numCoefs_ = *reinterpret_cast<const uint16_t*>(rawFmt + 20);
                if (numCoefs_ > 0 && cbSize >= 4 + numCoefs_ * 4) {
                    const auto* coefPtr = reinterpret_cast<const int16_t*>(rawFmt + 22);
                    for (uint32_t i = 0; i < numCoefs_ && i < 32; ++i) {
                        coef1_[i] = coefPtr[i * 2];
                        coef2_[i] = coefPtr[i * 2 + 1];
                    }
                }
            }
        }

        if (formatTag_ == 0x0161) {
            const uint8_t* extraData = nullptr;
            size_t extraDataSize = 0;
            if (pSourceFormat) {
                const auto* rawFmt = reinterpret_cast<const uint8_t*>(pSourceFormat);
                uint16_t cbSize = *reinterpret_cast<const uint16_t*>(rawFmt + 16);
                if (cbSize > 0) {
                    extraData = rawFmt + 18;
                    extraDataSize = cbSize;
                }
            }
            wmaDecoder_ = std::make_unique<sgre::WmaDecoder>();
            wmaDecoder_->init(sampleRate_, channels_, avgBytesPerSec_, blockAlign_, extraData, extraDataSize);
        }

        XAudio2MasterMixer::instance().registerVoice(this);
    }

    ~SourceVoiceInstance() {
        isDestroying_.store(true, std::memory_order_release);
        isPlaying_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = nullptr;
        }
        XAudio2MasterMixer::instance().unregisterVoice(this);
        flush(false);
    }

    uint32_t getChannels() const { return channels_; }
    uint32_t getSampleRate() const { return sampleRate_; }

    void setVolume(float v) {
        volume_.store(std::clamp(v, 0.0f, 4.0f), std::memory_order_relaxed);
    }

    float getVolume() const {
        return volume_.load(std::memory_order_relaxed);
    }

    bool isDestroying() const {
        return isDestroying_.load(std::memory_order_acquire);
    }

    bool hasActiveWork() const {
        return isPlaying_.load(std::memory_order_relaxed) &&
               buffersQueued_.load(std::memory_order_relaxed) > 0 &&
               !isDestroying_.load(std::memory_order_relaxed);
    }

    void start() {
        isPlaying_.store(true, std::memory_order_release);
        XAudio2MasterMixer::instance().notifyWorkAvailable();
    }

    void stop() {
        isPlaying_.store(false, std::memory_order_release);
    }

    void submitBuffer(const XAUDIO2_BUFFER_64* buf) {
        if (!buf || isDestroying_.load(std::memory_order_acquire)) return;

        const uint32_t outRate = XAudio2MasterMixer::instance().outputSampleRate();
        uint64_t estUs = (static_cast<uint64_t>(buf->AudioBytes) * 1000000ULL) / avgBytesPerSec_;
        if (estUs == 0) estUs = 10000ULL;

        std::vector<int16_t> decodedRaw;
        uint64_t sourceFrames = 0;

        {
            std::lock_guard<std::mutex> decLock(decodeMutex_);
            if (formatTag_ == 2 && buf->pAudioData && buf->AudioBytes > 0) {
                decodeMsAdpcm(
                    buf->pAudioData, buf->AudioBytes,
                    channels_, blockAlign_,
                    numCoefs_ > 0 ? coef1_ : nullptr,
                    numCoefs_ > 0 ? coef2_ : nullptr,
                    numCoefs_,
                    decodedRaw
                );
                sourceFrames = decodedRaw.size() / std::max<uint32_t>(1u, channels_);
            } else if (formatTag_ == 0x0161 && buf->pAudioData && buf->AudioBytes > 0) {
                if (buffersQueued_.load(std::memory_order_relaxed) == 0 && wmaDecoder_) {
                    wmaDecoder_->reset();
                }
                if (wmaDecoder_ && wmaDecoder_->isInitialized()) {
                    wmaDecoder_->decode(buf->pAudioData, buf->AudioBytes, decodedRaw);
                }
                if (decodedRaw.empty()) {
                    uint32_t fallbackFrames = static_cast<uint32_t>((sampleRate_ * estUs) / 1000000ULL);
                    if (fallbackFrames == 0) fallbackFrames = 1024;
                    decodedRaw.assign(static_cast<size_t>(fallbackFrames) * channels_, 0);
                }
                sourceFrames = decodedRaw.size() / std::max<uint32_t>(1u, channels_);
            } else if (formatTag_ == 1 && buf->pAudioData && buf->AudioBytes >= sizeof(int16_t)) {
                size_t totalInt16 = buf->AudioBytes / sizeof(int16_t);
                const auto* rawSamples = reinterpret_cast<const int16_t*>(buf->pAudioData);
                decodedRaw.assign(rawSamples, rawSamples + totalInt16);
                sourceFrames = totalInt16 / std::max<uint32_t>(1u, channels_);
            } else {
                uint32_t fallbackFrames = static_cast<uint32_t>((sampleRate_ * estUs) / 1000000ULL);
                if (fallbackFrames == 0) fallbackFrames = 256;
                decodedRaw.assign(static_cast<size_t>(fallbackFrames) * channels_, 0);
                sourceFrames = fallbackFrames;
            }
        }

        // Chuẩn hóa kênh âm thanh về Stereo 2 kênh
        std::vector<int16_t> stereoSrc;
        if (channels_ == 1) {
            stereoSrc.resize(decodedRaw.size() * 2u);
            for (size_t i = 0; i < decodedRaw.size(); ++i) {
                stereoSrc[i * 2u] = decodedRaw[i];
                stereoSrc[i * 2u + 1u] = decodedRaw[i];
            }
        } else if (channels_ == 2) {
            stereoSrc = std::move(decodedRaw);
        } else {
            size_t frames = decodedRaw.size() / channels_;
            stereoSrc.resize(frames * 2u);
            for (size_t f = 0; f < frames; ++f) {
                stereoSrc[f * 2u] = decodedRaw[f * channels_];
                stereoSrc[f * 2u + 1u] = decodedRaw[f * channels_ + 1u];
            }
        }

        PreparedAudioChunk chunk;
        chunk.pContext = buf->pContext;
        chunk.sourceFrames = sourceFrames > 0 ? sourceFrames : (stereoSrc.size() / 2u);
        resampleLinearStereo(stereoSrc.data(), stereoSrc.size() / 2u, sampleRate_, outRate, chunk.stereoSamples);
        chunk.totalMixFrames = chunk.stereoSamples.size() / 2u;

        // Soft Ramp chống tiếng bụp đầu câu thoại WMA/PCM (không áp dụng cho SE MS-ADPCM formatTag == 2)
        if (formatTag_ != 2 && needSoftRamp_.exchange(false) && chunk.totalMixFrames >= 16u) {
            size_t rampFrames = std::min<size_t>(chunk.totalMixFrames, 256u);
            for (size_t f = 0; f < rampFrames; ++f) {
                float factor = static_cast<float>(f) / static_cast<float>(rampFrames);
                chunk.stereoSamples[f * 2u] = static_cast<int16_t>(chunk.stereoSamples[f * 2u] * factor);
                chunk.stereoSamples[f * 2u + 1u] = static_cast<int16_t>(chunk.stereoSamples[f * 2u + 1u] * factor);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            chunkQueue_.push_back(std::move(chunk));
            buffersQueued_.fetch_add(1, std::memory_order_relaxed);
            if (currentContext_ == nullptr) {
                currentContext_ = chunkQueue_.front().pContext;
            }
        }
        XAudio2MasterMixer::instance().notifyWorkAvailable();
    }

    void flush(bool triggerCallbacks = true) {
        needSoftRamp_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> decLock(decodeMutex_);
            if (wmaDecoder_) {
                wmaDecoder_->reset();
            }
        }
        std::vector<void*> flushedContexts;
        void* cbPtr = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cbPtr = callback_;
            while (!chunkQueue_.empty()) {
                flushedContexts.push_back(chunkQueue_.front().pContext);
                chunkQueue_.pop_front();
            }
            buffersQueued_.store(0, std::memory_order_relaxed);
            currentContext_ = nullptr;
        }
        if (triggerCallbacks && cbPtr && !isDestroying_.load(std::memory_order_acquire)) {
            void** vtbl = *reinterpret_cast<void***>(cbPtr);
            auto fnEnd = reinterpret_cast<FnOnBufferEnd>(vtbl[4]);
            for (void* ctx : flushedContexts) {
                fnEnd(cbPtr, ctx);
            }
        }
    }

    void getState(XAUDIO2_VOICE_STATE_64* state) {
        if (!state) return;
        std::lock_guard<std::mutex> lock(mutex_);
        state->BuffersQueued = buffersQueued_.load(std::memory_order_relaxed);
        state->SamplesPlayed = samplesPlayed_.load(std::memory_order_relaxed);
        state->pCurrentBufferContext = currentContext_;
    }

    // Trộn tối đa framesToMix khung mẫu vào bộ đệm tích lũy 32-bit của Mixer
    bool mixInto(int32_t* accumStereo, size_t framesToMix, float masterVol, std::vector<PendingVoiceCallback>& outCallbacks) {
        if (!isPlaying_.load(std::memory_order_acquire) || isDestroying_.load(std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (chunkQueue_.empty()) {
            return false;
        }

        const float gain = volume_.load(std::memory_order_relaxed) * masterVol;
        size_t mixedFrames = 0;

        while (mixedFrames < framesToMix && !chunkQueue_.empty()) {
            PreparedAudioChunk& front = chunkQueue_.front();
            if (!front.started) {
                front.started = true;
                currentContext_ = front.pContext;
                if (callback_ != nullptr) {
                    outCallbacks.push_back({this, callback_, front.pContext, true});
                }
            }

            const size_t avail = front.totalMixFrames - front.cursorFrame;
            const size_t take = std::min(framesToMix - mixedFrames, avail);
            const int16_t* srcPtr = front.stereoSamples.data() + front.cursorFrame * 2u;
            int32_t* dstPtr = accumStereo + mixedFrames * 2u;

            if (std::abs(gain - 1.0f) <= 0.001f) {
                for (size_t i = 0; i < take * 2u; ++i) {
                    dstPtr[i] += static_cast<int32_t>(srcPtr[i]);
                }
            } else if (gain > 0.0001f) {
                for (size_t i = 0; i < take * 2u; ++i) {
                    dstPtr[i] += static_cast<int32_t>(std::lround(static_cast<float>(srcPtr[i]) * gain));
                }
            }

            front.cursorFrame += take;
            mixedFrames += take;

            if (front.cursorFrame >= front.totalMixFrames) {
                void* doneCtx = front.pContext;
                samplesPlayed_.fetch_add(front.sourceFrames, std::memory_order_relaxed);
                if (buffersQueued_.load(std::memory_order_relaxed) > 0) {
                    buffersQueued_.fetch_sub(1, std::memory_order_relaxed);
                }
                chunkQueue_.pop_front();
                currentContext_ = chunkQueue_.empty() ? nullptr : chunkQueue_.front().pContext;
                if (callback_ != nullptr) {
                    outCallbacks.push_back({this, callback_, doneCtx, false});
                }
            }
        }

        return mixedFrames > 0;
    }

private:
    void* vtable_[32] = { nullptr };
    void* callback_ = nullptr;
    uint32_t avgBytesPerSec_ = 192000;
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t bitsPerSample_ = 16;
    uint16_t formatTag_ = 1;
    uint16_t blockAlign_ = 4;
    uint16_t samplesPerBlock_ = 0;
    uint16_t numCoefs_ = 0;
    int32_t coef1_[32]{ 0 };
    int32_t coef2_[32]{ 0 };
    std::atomic<float> volume_{ 1.0f };
    std::unique_ptr<sgre::WmaDecoder> wmaDecoder_;

    std::mutex mutex_;
    std::mutex decodeMutex_;
    std::deque<PreparedAudioChunk> chunkQueue_;
    std::atomic<bool> isPlaying_{ false };
    std::atomic<bool> isDestroying_{ false };
    std::atomic<bool> needSoftRamp_{ true };
    std::atomic<uint32_t> buffersQueued_{ 0 };
    std::atomic<uint64_t> samplesPlayed_{ 0 };
    void* currentContext_ = nullptr;

    void initVtable() {
        for (size_t i = 0; i < 32; ++i) vtable_[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
        vtable_[0] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_GetVoiceDetails);
        vtable_[12] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_SetVolume);
        vtable_[13] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_GetVolume);
        vtable_[18] = reinterpret_cast<void*>(&SourceVoiceInstance::Voice_DestroyVoice);
        vtable_[19] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_Start);
        vtable_[20] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_Stop);
        vtable_[21] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_SubmitSourceBuffer);
        vtable_[22] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_FlushSourceBuffers);
        vtable_[25] = reinterpret_cast<void*>(&SourceVoiceInstance::SourceVoice_GetState);
    }

    static void WINAPI SourceVoice_GetVoiceDetails(void* thisPtr, void* pDetails) {
        if (pDetails) {
            auto* inst = reinterpret_cast<SourceVoiceInstance*>(thisPtr);
            auto* details = reinterpret_cast<XAUDIO2_VOICE_DETAILS*>(pDetails);
            details->CreationFlags = 0;
            details->ActiveFlags = 0;
            details->InputChannels = inst->getChannels();
            details->InputSampleRate = inst->getSampleRate();
        }
    }

    static int32_t WINAPI SourceVoice_SetVolume(void* thisPtr, float volume, uint32_t) {
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->setVolume(volume);
        return 0;
    }

    static void WINAPI SourceVoice_GetVolume(void* thisPtr, float* pVolume) {
        if (pVolume) {
            *pVolume = reinterpret_cast<SourceVoiceInstance*>(thisPtr)->getVolume();
        }
    }

    static void WINAPI Voice_DestroyVoice(void* thisPtr) {
        LOGI("IXAudio2SourceVoice::DestroyVoice called: %p", thisPtr);
        delete reinterpret_cast<SourceVoiceInstance*>(thisPtr);
    }

    static int32_t WINAPI SourceVoice_Start(void* thisPtr, uint32_t flags, uint32_t opSet) {
        (void)flags;
        (void)opSet;
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->start();
        return 0;
    }

    static int32_t WINAPI SourceVoice_Stop(void* thisPtr, uint32_t flags, uint32_t opSet) {
        (void)flags;
        (void)opSet;
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->stop();
        return 0;
    }

    static int32_t WINAPI SourceVoice_SubmitSourceBuffer(void* thisPtr, const void* pBuffer, const void* /* pBufferWMA */) {
        auto* inst = reinterpret_cast<SourceVoiceInstance*>(thisPtr);
        auto* buf = reinterpret_cast<const XAUDIO2_BUFFER_64*>(pBuffer);
        inst->submitBuffer(buf);
        return 0;
    }

    static int32_t WINAPI SourceVoice_FlushSourceBuffers(void* thisPtr) {
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->flush();
        return 0;
    }

    static void WINAPI SourceVoice_GetState(void* thisPtr, void* pVoiceState, uint32_t) {
        auto* state = reinterpret_cast<XAUDIO2_VOICE_STATE_64*>(pVoiceState);
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->getState(state);
    }
};

void XAudio2MasterMixer::mixerLoop() {
    Win32Bridge_SetupCurrentThreadTEB();

    int32_t accumBuf[kMixBlockSamples];
    int16_t outBuf[kMixBlockSamples];
    std::vector<PendingVoiceCallback> pendingCallbacks;
    pendingCallbacks.reserve(16);

    while (running_.load(std::memory_order_acquire)) {
        if (lifecyclePaused_.load(std::memory_order_relaxed)) {
            if (stream_) {
                aaudio_stream_state_t st = AAudioStream_getState(stream_);
                if (st == AAUDIO_STREAM_STATE_STARTED || st == AAUDIO_STREAM_STATE_STARTING) {
                    AAudioStream_requestPause(stream_);
                }
            }
            std::unique_lock<std::mutex> lock(voicesMutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                return !running_.load(std::memory_order_relaxed) || !lifecyclePaused_.load(std::memory_order_relaxed);
            });
            continue;
        }

        bool anyVoiceReady = false;
        {
            std::unique_lock<std::mutex> lock(voicesMutex_);
            for (const auto* v : voices_) {
                if (v && v->hasActiveWork()) {
                    anyVoiceReady = true;
                    break;
                }
            }
            if (!anyVoiceReady) {
                cv_.wait_for(lock, std::chrono::milliseconds(15), [&]() {
                    if (!running_.load(std::memory_order_relaxed) || lifecyclePaused_.load(std::memory_order_relaxed)) {
                        return true;
                    }
                    for (const auto* v : voices_) {
                        if (v && v->hasActiveWork()) return true;
                    }
                    return false;
                });
                continue;
            }
        }

        std::memset(accumBuf, 0, sizeof(accumBuf));
        pendingCallbacks.clear();
        bool mixedAny = false;
        const float masterVol = masterVolume_.load(std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(voicesMutex_);
            for (SourceVoiceInstance* v : voices_) {
                if (v && v->mixInto(accumBuf, kMixBlockFrames, masterVol, pendingCallbacks)) {
                    mixedAny = true;
                }
            }
        }

        if (mixedAny) {
            for (size_t i = 0; i < kMixBlockSamples; ++i) {
                outBuf[i] = clampPcm16(accumBuf[i]);
            }

            if (stream_) {
                aaudio_stream_state_t st = AAudioStream_getState(stream_);
                if (st == AAUDIO_STREAM_STATE_DISCONNECTED) {
                    LOGW("XAudio2MasterMixer: Thiết bị âm thanh thay đổi (tai nghe/Bluetooth), mở lại AAudioStream...");
                    openSharedStream();
                    st = stream_ ? AAudioStream_getState(stream_) : AAUDIO_STREAM_STATE_UNINITIALIZED;
                }
                if (stream_ && st != AAUDIO_STREAM_STATE_STARTED && st != AAUDIO_STREAM_STATE_STARTING) {
                    AAudioStream_requestStart(stream_);
                }
                if (stream_) {
                    int32_t remaining = static_cast<int32_t>(kMixBlockFrames);
                    const int16_t* writePtr = outBuf;
                    while (remaining > 0 && running_.load(std::memory_order_relaxed) && !lifecyclePaused_.load(std::memory_order_relaxed)) {
                        aaudio_result_t wr = AAudioStream_write(stream_, writePtr, remaining, 50000000LL);
                        if (wr > 0) {
                            remaining -= wr;
                            writePtr += wr * kMasterChannels;
                        } else if (wr == AAUDIO_ERROR_DISCONNECTED) {
                            openSharedStream();
                            break;
                        } else {
                            break;
                        }
                    }
                }
            } else {
                const uint32_t rate = std::max<uint32_t>(8000u, actualSampleRate_.load(std::memory_order_relaxed));
                const uint64_t sleepUs = (static_cast<uint64_t>(kMixBlockFrames) * 1000000ULL) / rate;
                std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
            }
        }

        // Gọi OnBufferStart / OnBufferEnd bên ngoài khóa của voice để tránh hoàn toàn deadlock tái nhập
        for (const auto& cb : pendingCallbacks) {
            {
                std::lock_guard<std::mutex> lock(voicesMutex_);
                if (std::find(voices_.begin(), voices_.end(), cb.voice) == voices_.end() || cb.voice->isDestroying()) {
                    continue;
                }
                activeCallbackVoice_ = cb.voice;
            }

            if (cb.callbackPtr && !cb.voice->isDestroying()) {
                void** vtbl = *reinterpret_cast<void***>(cb.callbackPtr);
                if (cb.isBufferStart) {
                    reinterpret_cast<FnOnBufferStart>(vtbl[3])(cb.callbackPtr, cb.pContext);
                } else {
                    reinterpret_cast<FnOnBufferEnd>(vtbl[4])(cb.callbackPtr, cb.pContext);
                }
            }

            {
                std::lock_guard<std::mutex> lock(voicesMutex_);
                activeCallbackVoice_ = nullptr;
            }
            callbackDoneCv_.notify_all();
        }
    }
}

// --- Mock Mastering Voice ---
static void* gMasteringVoiceVtable[32];
static void* gMockMasteringVoice = nullptr;

static void WINAPI MasteringVoice_GetVoiceDetails(void*, void* pDetails) {
    if (pDetails) {
        auto* details = reinterpret_cast<XAUDIO2_VOICE_DETAILS*>(pDetails);
        details->CreationFlags = 0;
        details->ActiveFlags = 0;
        details->InputChannels = kMasterChannels;
        details->InputSampleRate = XAudio2MasterMixer::instance().outputSampleRate();
    }
}

static int32_t WINAPI MasteringVoice_SetVolume(void*, float volume, uint32_t) {
    XAudio2MasterMixer::instance().setMasterVolume(volume);
    LOGI("IXAudio2MasteringVoice::SetVolume -> %.3f", volume);
    return 0;
}

static void WINAPI MasteringVoice_GetVolume(void*, float* pVolume) {
    if (pVolume) {
        *pVolume = XAudio2MasterMixer::instance().getMasterVolume();
    }
}

static void initMasteringVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) gMasteringVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    gMasteringVoiceVtable[0] = reinterpret_cast<void*>(&MasteringVoice_GetVoiceDetails);
    gMasteringVoiceVtable[12] = reinterpret_cast<void*>(&MasteringVoice_SetVolume);
    gMasteringVoiceVtable[13] = reinterpret_cast<void*>(&MasteringVoice_GetVolume);
    gMockMasteringVoice = &gMasteringVoiceVtable[0];
    initialized = true;
}

// --- Mock Submix Voice ---
static void* gSubmixVoiceVtable[32];
static void* gMockSubmixVoice = nullptr;

static void initSubmixVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) gSubmixVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    gMockSubmixVoice = &gSubmixVoiceVtable[0];
    initialized = true;
}

// --- Mock IXAudio2 ---
static void* gXAudio2Vtable[32];
static void* gMockXAudio2 = nullptr;

static int32_t WINAPI xaudio_QueryInterface(void*, const void*, void** ppvObject) {
    if (ppvObject) *ppvObject = &gMockXAudio2;
    return 0;
}
static uint32_t WINAPI xaudio_AddRef(void*) { return 1; }
static uint32_t WINAPI xaudio_Release(void*) { return 1; }

static int32_t WINAPI xaudio_CreateSourceVoice(
    void*, void** ppSourceVoice, const void* pSourceFormat, uint32_t, float,
    void* pCallback, const void*, const void*
) {
    uint16_t formatTag = 1;
    uint32_t avgBytesPerSec = 192000;
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bitsPerSample = 16;
    uint16_t blockAlign = 4;
    uint16_t cbSize = 0;

    if (pSourceFormat) {
        const auto* rawFmt = reinterpret_cast<const uint8_t*>(pSourceFormat);
        formatTag = *reinterpret_cast<const uint16_t*>(rawFmt + 0);
        channels = *reinterpret_cast<const uint16_t*>(rawFmt + 2);
        sampleRate = *reinterpret_cast<const uint32_t*>(rawFmt + 4);
        avgBytesPerSec = *reinterpret_cast<const uint32_t*>(rawFmt + 8);
        blockAlign = *reinterpret_cast<const uint16_t*>(rawFmt + 12);
        bitsPerSample = *reinterpret_cast<const uint16_t*>(rawFmt + 14);
        if (formatTag != 1) {
            cbSize = *reinterpret_cast<const uint16_t*>(rawFmt + 16);
        }
    }
    LOGI("IXAudio2::CreateSourceVoice -> tag=0x%04X, ch=%u, rate=%u, avgBps=%u, align=%u, bits=%u, cbSize=%u",
         formatTag, channels, sampleRate, avgBytesPerSec, blockAlign, bitsPerSample, cbSize);

    auto* voiceInst = new SourceVoiceInstance(
        pCallback, avgBytesPerSec, sampleRate, channels, bitsPerSample, formatTag, blockAlign, pSourceFormat
    );
    if (ppSourceVoice) {
        *ppSourceVoice = voiceInst;
    }
    return 0;
}

static int32_t WINAPI xaudio_CreateSubmixVoice(
    void*, void** ppSubmixVoice, uint32_t, uint32_t, uint32_t, uint32_t, const void*, const void*
) {
    initSubmixVoiceVtable();
    if (ppSubmixVoice) *ppSubmixVoice = &gMockSubmixVoice;
    return 0;
}

static int32_t WINAPI xaudio_CreateMasteringVoice(
    void*, void** ppMasteringVoice, uint32_t, uint32_t, uint32_t, const void*, const void*, uint32_t
) {
    initMasteringVoiceVtable();
    if (ppMasteringVoice) *ppMasteringVoice = &gMockMasteringVoice;
    return 0;
}

static void initXAudio2Vtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) gXAudio2Vtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    gXAudio2Vtable[0] = reinterpret_cast<void*>(&xaudio_QueryInterface);
    gXAudio2Vtable[1] = reinterpret_cast<void*>(&xaudio_AddRef);
    gXAudio2Vtable[2] = reinterpret_cast<void*>(&xaudio_Release);
    gXAudio2Vtable[5] = reinterpret_cast<void*>(&xaudio_CreateSourceVoice);
    gXAudio2Vtable[6] = reinterpret_cast<void*>(&xaudio_CreateSubmixVoice);
    gXAudio2Vtable[7] = reinterpret_cast<void*>(&xaudio_CreateMasteringVoice);
    gXAudio2Vtable[8] = reinterpret_cast<void*>(&xaudio_CreateSubmixVoice);
    gMockXAudio2 = &gXAudio2Vtable[0];
    initialized = true;
}

} // namespace

extern "C" {

int32_t WINAPI XAudio2Create(void** ppXAudio2, uint32_t Flags, uint32_t XAudio2Processor) {
    LOGI("XAudio2Create intercepted: Flags=0x%X, Processor=0x%X, ppXAudio2=%p", Flags, XAudio2Processor, ppXAudio2);
    if (!ppXAudio2) return 0x80070057; // E_INVALIDARG
    initXAudio2Vtable();
    *ppXAudio2 = &gMockXAudio2;
    return 0;
}

void XAudio2Bridge_SetLifecyclePaused(bool paused) {
    XAudio2MasterMixer::instance().setLifecyclePaused(paused);
}

} // extern "C"
