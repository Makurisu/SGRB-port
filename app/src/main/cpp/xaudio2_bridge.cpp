#include "xaudio2_bridge.h"
#include "wma_decoder.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <cstring>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <memory>

#define LOG_TAG "SGRE_XAudio2Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

extern "C" bool Win32Bridge_SetupCurrentThreadTEB();

namespace {

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
    uint32_t pad; // Đảm bảo căn chỉnh 8-byte cho con trỏ 64-bit pContext
    void* pContext;
};

struct XAUDIO2_VOICE_STATE_64 {
    void* pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};

// Bố cục vtable của IXAudio2VoiceCallback trong game (IDA 0x14038E720, 0x14038E670)
// Slot 0..2: IUnknown
// Slot 3 (offset 24 = 0x18): OnBufferStart(void* pBufferContext)
// Slot 4 (offset 32 = 0x20): OnBufferEnd(void* pBufferContext)
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

static inline int16_t msadpcm_clamp16(int32_t val) {
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
    int16_t clamped = msadpcm_clamp16(newSample);

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

                int16_t sL = msadpcm_decode_nibble(nL, c1_0, c2_0, delta0, samp1_0, samp2_0);
                int16_t sR = msadpcm_decode_nibble(nR, c1_1, c2_1, delta1, samp1_1, samp2_1);
                outPcm.push_back(sL);
                outPcm.push_back(sR);
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

static void resampleLinear(const int16_t* inSamples, size_t inFrames, uint32_t inRate,
                           uint32_t outRate, uint32_t channels, std::vector<int16_t>& outSamples) {
    if (!inSamples || inFrames == 0 || inRate == 0 || outRate == 0 || channels == 0) return;
    if (inRate == outRate) {
        outSamples.assign(inSamples, inSamples + inFrames * channels);
        return;
    }
    size_t outFrames = (inFrames * outRate) / inRate;
    outSamples.resize(outFrames * channels);
    double ratio = static_cast<double>(inRate) / static_cast<double>(outRate);

    for (size_t i = 0; i < outFrames; ++i) {
        double srcPos = i * ratio;
        size_t idx0 = static_cast<size_t>(srcPos);
        size_t idx1 = std::min(idx0 + 1, inFrames - 1);
        double frac = srcPos - static_cast<double>(idx0);

        for (uint32_t ch = 0; ch < channels; ++ch) {
            double s0 = inSamples[idx0 * channels + ch];
            double s1 = inSamples[idx1 * channels + ch];
            double s = s0 + frac * (s1 - s0);
            s = std::max(-32768.0, std::min(32767.0, s));
            outSamples[i * channels + ch] = static_cast<int16_t>(s);
        }
    }
}

struct AudioBufferTask {
    void* pContext = nullptr;
    uint32_t audioBytes = 0;
    std::vector<uint8_t> pcmData; // Bản copy sâu của dữ liệu âm thanh để an toàn bộ nhớ tuyệt đối
    const uint8_t* pAudioData = nullptr;
    std::chrono::microseconds durationUs{ 0 };
    std::vector<int16_t> decodedPcm;
};

class SourceVoiceInstance {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

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
        // Luôn mở AAudioStream với 2 kênh Stereo (nếu nguồn là Mono, ta upmix sang Stereo khi playback)
        // để tương thích hoàn hảo 100% với Android HAL audio và LDPlayer/thiết bị thật không bao giờ bị rè hay vỡ tiếng
        streamChannels_ = (channels_ == 1) ? 2 : channels_;
        pcmBytesPerFrame_ = streamChannels_ * sizeof(int16_t);
        initVtable();
        lpVtbl = vtable_;

        // Đọc hệ số tùy chỉnh của MS-ADPCM nếu có
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

        // Khởi tạo bộ giải mã WMA v2 (0x0161) cho voice nhân vật
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

        // Khởi tạo luồng AAudio độc lập cho mỗi source voice trong chế độ chia sẻ SHARED
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t res = AAudio_createStreamBuilder(&builder);
        if (res == AAUDIO_OK && builder) {
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            // Luôn yêu cầu tần số phần cứng chuẩn 48000 Hz để tránh AudioFlinger tạo phần mềm resampler lỗi
            AAudioStreamBuilder_setSampleRate(builder, 48000);
            AAudioStreamBuilder_setChannelCount(builder, streamChannels_);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

            res = AAudioStreamBuilder_openStream(builder, &stream_);
            AAudioStreamBuilder_delete(builder);

            if (res == AAUDIO_OK && stream_) {
                int32_t cap = AAudioStream_getBufferCapacityInFrames(stream_);
                if (cap > 0) {
                    AAudioStream_setBufferSizeInFrames(stream_, cap);
                }
                actualSampleRate_ = static_cast<uint32_t>(AAudioStream_getSampleRate(stream_));
                LOGI("SourceVoiceInstance: AAudio stream opened: Rate=%u (Req=%u), Channels=%d (SrcCh=%d), BufferSize=%d, FormatTag=0x%04X",
                     actualSampleRate_, sampleRate_, AAudioStream_getChannelCount(stream_), channels_,
                     AAudioStream_getBufferSizeInFrames(stream_), formatTag_);
            } else {
                LOGW("SourceVoiceInstance: openStream failed: %d (%s), will fallback to virtual clock",
                     res, AAudio_convertResultToText(res));
                stream_ = nullptr;
            }
        }

        isRunning_ = true;
        playbackThread_ = std::thread(&SourceVoiceInstance::playbackLoop, this);
    }

    ~SourceVoiceInstance() {
        stopThread();
    }

    void* getVtable() { return vtable_; }
    uint32_t getChannels() const { return channels_; }
    uint32_t getSampleRate() const { return sampleRate_; }

    void setVolume(float v) {
        volume_ = v;
    }

    float getVolume() const {
        return volume_;
    }

    void start() {
        isPlaying_ = true;
        // Bỏ AAudioStream_requestStart ở đây vì playbackLoop sẽ kích hoạt stream
        // ngay khi buffer dữ liệu đầu tiên sẵn sàng, loại bỏ hoàn toàn race condition -895
        cv_.notify_all();
    }

    void stop() {
        isPlaying_ = false;
        if (stream_) {
            AAudioStream_requestPause(stream_);
        }
    }

    void submitBuffer(const XAUDIO2_BUFFER_64* buf) {
        if (!buf) return;
        AudioBufferTask task;
        task.pContext = buf->pContext;
        task.audioBytes = buf->AudioBytes;
        if (buf->pAudioData && buf->AudioBytes > 0) {
            task.pcmData.assign(buf->pAudioData, buf->pAudioData + buf->AudioBytes);
            task.pAudioData = task.pcmData.data();
        } else {
            task.pAudioData = nullptr;
        }
        uint64_t us = (static_cast<uint64_t>(buf->AudioBytes) * 1000000ULL) / avgBytesPerSec_;
        task.durationUs = std::chrono::microseconds(us > 0 ? us : 10000);

        // Tự động giải mã định dạng MS-ADPCM nén sang Linear PCM 16-bit
        if (formatTag_ == 2 && task.pAudioData && task.audioBytes > 0) {
            decodeMsAdpcm(
                task.pAudioData, task.audioBytes,
                channels_, blockAlign_,
                numCoefs_ > 0 ? coef1_ : nullptr,
                numCoefs_ > 0 ? coef2_ : nullptr,
                numCoefs_,
                task.decodedPcm
            );
        } else if (formatTag_ == 0x0161 && task.pAudioData && task.audioBytes > 0) {
            // Tự động giải mã định dạng WMA v2 nén sang Linear PCM 16-bit
            if (buffersQueued_.load() == 0 && wmaDecoder_) {
                wmaDecoder_->reset();
            }
            if (wmaDecoder_ && wmaDecoder_->isInitialized()) {
                wmaDecoder_->decode(task.pAudioData, task.audioBytes, task.decodedPcm);
            }
            int16_t s0 = task.decodedPcm.size() > 0 ? task.decodedPcm[0] : 0;
            int16_t s1 = task.decodedPcm.size() > 1 ? task.decodedPcm[1] : 0;
            int16_t s2 = task.decodedPcm.size() > 2 ? task.decodedPcm[2] : 0;
            LOGI("WmaDecoder: inBytes=%u -> outSamples=%zu, firstSamples=[%d, %d, %d]",
                 task.audioBytes, task.decodedPcm.size(), s0, s1, s2);
            // Dự phòng an toàn tuyệt đối: nếu WMA packet giải mã rỗng, tạo PCM silence để voice không bị kẹt hay crash
            if (task.decodedPcm.empty()) {
                uint32_t fallbackFrames = (sampleRate_ * static_cast<uint32_t>(task.durationUs.count())) / 1000000;
                if (fallbackFrames == 0) fallbackFrames = 1024;
                task.decodedPcm.assign(fallbackFrames * channels_, 0);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        taskQueue_.push(std::move(task));
        buffersQueued_++;
        cv_.notify_one();
    }

    void flush(bool triggerCallbacks = true) {
        needSoftRamp_ = true;
        if (wmaDecoder_) {
            wmaDecoder_->reset();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        while (!taskQueue_.empty()) {
            auto task = taskQueue_.front();
            taskQueue_.pop();
            if (triggerCallbacks && !isDestroying_.load()) {
                callOnBufferEnd(task.pContext);
            }
        }
        buffersQueued_ = 0;
    }

    void getState(XAUDIO2_VOICE_STATE_64* state) {
        if (!state) return;
        std::lock_guard<std::mutex> lock(mutex_);
        state->BuffersQueued = buffersQueued_.load();
        state->SamplesPlayed = samplesPlayed_.load();
        state->pCurrentBufferContext = currentContext_;
    }

private:
    void* vtable_[32] = { nullptr };
    void* callback_ = nullptr;
    uint32_t avgBytesPerSec_ = 192000;
    uint32_t sampleRate_ = 48000;
    uint32_t actualSampleRate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t streamChannels_ = 2;
    uint32_t bitsPerSample_ = 16;
    uint32_t bytesPerFrame_ = 4;
    uint16_t formatTag_ = 1;
    uint16_t blockAlign_ = 4;
    uint16_t samplesPerBlock_ = 0;
    uint16_t numCoefs_ = 0;
    int32_t coef1_[32]{ 0 };
    int32_t coef2_[32]{ 0 };
    uint32_t pcmBytesPerFrame_ = 4;
    float volume_ = 1.0f;
    std::unique_ptr<sgre::WmaDecoder> wmaDecoder_;

    AAudioStream* stream_ = nullptr;
    std::thread playbackThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<AudioBufferTask> taskQueue_;
    std::atomic<bool> isRunning_{ false };
    std::atomic<bool> isPlaying_{ false };
    std::atomic<bool> isDestroying_{ false };
    std::atomic<bool> needSoftRamp_{ true };
    std::atomic<uint32_t> buffersQueued_{ 0 };
    std::atomic<uint64_t> samplesPlayed_{ 0 };
    void* currentContext_ = nullptr;

    void stopThread() {
        if (!isDestroying_.exchange(true)) {
            isRunning_ = false;
            callback_ = nullptr; // Ngắt callback ngay lập tức để tránh deadlock với Game Thread
            cv_.notify_all();
            if (stream_) {
                AAudioStream_requestStop(stream_);
                AAudioStream_close(stream_);
                stream_ = nullptr;
            }
            if (playbackThread_.joinable()) {
                playbackThread_.join();
            }
            flush(false); // Dọn sạch buffer mà không kích hoạt callback khi voice đang hủy
        }
    }

    void callOnBufferStart(void* pContext) {
        if (callback_ && !isDestroying_.load()) {
            void** vtbl = *reinterpret_cast<void***>(callback_);
            auto fn = reinterpret_cast<FnOnBufferStart>(vtbl[3]);
            fn(callback_, pContext);
        }
    }

    void callOnBufferEnd(void* pContext) {
        if (callback_ && !isDestroying_.load()) {
            void** vtbl = *reinterpret_cast<void***>(callback_);
            auto fn = reinterpret_cast<FnOnBufferEnd>(vtbl[4]);
            fn(callback_, pContext);
        }
    }

    void playbackLoop() {
        Win32Bridge_SetupCurrentThreadTEB();
        static uint32_t sBufferCount = 0;

        while (isRunning_.load() && !isDestroying_.load()) {
            AudioBufferTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return !isRunning_.load() || isDestroying_.load() || (isPlaying_.load() && !taskQueue_.empty());
                });

                if (!isRunning_.load() || isDestroying_.load()) break;
                if (!isPlaying_.load() || taskQueue_.empty()) continue;

                task = taskQueue_.front();
                taskQueue_.pop();
                currentContext_ = task.pContext;
            }

            if (!isRunning_.load() || isDestroying_.load()) break;

            // Gọi OnBufferStart để cập nhật QPC timestamp
            callOnBufferStart(task.pContext);

            // Ghi âm thanh ra phần cứng qua AAudio (hỗ trợ cả Linear PCM 16-bit, MS-ADPCM và WMA v2 đã giải mã)
            std::vector<int16_t> stereoPcm;
            const uint8_t* pcm = nullptr;
            int32_t framesRemaining = 0;
            int32_t initialFrames = 0;

            if ((formatTag_ == 2 || formatTag_ == 0x0161) && !task.decodedPcm.empty()) {
                if (channels_ == 1 && streamChannels_ == 2) {
                    // Upmix Mono sang Stereo chuẩn để âm thanh không bị rè hoặc lệch stride trên Android HAL
                    stereoPcm.resize(task.decodedPcm.size() * 2);
                    for (size_t i = 0; i < task.decodedPcm.size(); ++i) {
                        stereoPcm[i * 2] = task.decodedPcm[i];
                        stereoPcm[i * 2 + 1] = task.decodedPcm[i];
                    }
                    pcm = reinterpret_cast<const uint8_t*>(stereoPcm.data());
                    framesRemaining = static_cast<int32_t>(task.decodedPcm.size());
                } else {
                    pcm = reinterpret_cast<const uint8_t*>(task.decodedPcm.data());
                    framesRemaining = static_cast<int32_t>(task.decodedPcm.size() / channels_);
                }
                initialFrames = framesRemaining;
            } else if (task.pAudioData && task.audioBytes > 0 && formatTag_ == 1) {
                if (channels_ == 1 && streamChannels_ == 2) {
                    const auto* monoSamples = reinterpret_cast<const int16_t*>(task.pAudioData);
                    size_t count = task.audioBytes / sizeof(int16_t);
                    stereoPcm.resize(count * 2);
                    for (size_t i = 0; i < count; ++i) {
                        stereoPcm[i * 2] = monoSamples[i];
                        stereoPcm[i * 2 + 1] = monoSamples[i];
                    }
                    pcm = reinterpret_cast<const uint8_t*>(stereoPcm.data());
                    framesRemaining = static_cast<int32_t>(count);
                } else {
                    pcm = task.pAudioData;
                    framesRemaining = static_cast<int32_t>(task.audioBytes / pcmBytesPerFrame_);
                }
                initialFrames = framesRemaining;
            }

            // Tự động resample tuyến tính nếu tần số phần cứng AAudio khác với tần số nguồn (vd: 44.1kHz -> 48kHz)
            // triệt tiêu hoàn toàn hiện tượng méo tiếng / rè tiếng trên Android HAL
            std::vector<int16_t> resampledPcm;
            if (actualSampleRate_ > 0 && actualSampleRate_ != sampleRate_ && framesRemaining > 0 && pcm != nullptr) {
                resampleLinear(
                    reinterpret_cast<const int16_t*>(pcm),
                    static_cast<size_t>(framesRemaining),
                    sampleRate_,
                    actualSampleRate_,
                    streamChannels_,
                    resampledPcm
                );
                pcm = reinterpret_cast<const uint8_t*>(resampledPcm.data());
                framesRemaining = static_cast<int32_t>(resampledPcm.size() / streamChannels_);
            }

            if (pcm && framesRemaining > 0 && stream_ && pcmBytesPerFrame_ > 0) {
                // Áp dụng Soft Ramp (Linear Fade-In) cho voice thoại WMA v2 để triệt tiêu tiếng pop/crackle/rè.
                // TUYỆT ĐỐI KHÔNG áp dụng cho Sound Effect MS-ADPCM (formatTag_ == 2) để bảo toàn
                // trọn vẹn xung lực đỉnh (transient attack) của tiếng va chạm kim loại và tiếng nổ!
                if (formatTag_ != 2 && needSoftRamp_.exchange(false) && framesRemaining >= 16) {
                    if (pcm != reinterpret_cast<const uint8_t*>(stereoPcm.data()) &&
                        pcm != reinterpret_cast<const uint8_t*>(resampledPcm.data())) {
                        stereoPcm.assign(reinterpret_cast<const int16_t*>(pcm),
                                         reinterpret_cast<const int16_t*>(pcm) + framesRemaining * streamChannels_);
                        pcm = reinterpret_cast<const uint8_t*>(stereoPcm.data());
                    }
                    auto* rampBuf = reinterpret_cast<int16_t*>(const_cast<uint8_t*>(pcm));
                    size_t rampCount = std::min(static_cast<size_t>(framesRemaining), static_cast<size_t>(256));
                    for (size_t f = 0; f < rampCount; ++f) {
                        float factor = static_cast<float>(f) / static_cast<float>(rampCount);
                        for (uint32_t ch = 0; ch < streamChannels_; ++ch) {
                            rampBuf[f * streamChannels_ + ch] = static_cast<int16_t>(rampBuf[f * streamChannels_ + ch] * factor);
                        }
                    }
                }

                // Áp dụng âm lượng (Volume scaling) nếu volume khác 1.0f
                float curVol = volume_;
                if (curVol < 0.999f || curVol > 1.001f) {
                    if (pcm != reinterpret_cast<const uint8_t*>(stereoPcm.data()) &&
                        pcm != reinterpret_cast<const uint8_t*>(resampledPcm.data())) {
                        stereoPcm.assign(reinterpret_cast<const int16_t*>(pcm),
                                         reinterpret_cast<const int16_t*>(pcm) + framesRemaining * streamChannels_);
                        pcm = reinterpret_cast<const uint8_t*>(stereoPcm.data());
                    }
                    auto* volBuf = reinterpret_cast<int16_t*>(const_cast<uint8_t*>(pcm));
                    size_t sampleCount = static_cast<size_t>(framesRemaining) * streamChannels_;
                    for (size_t s = 0; s < sampleCount; ++s) {
                        int32_t scaled = static_cast<int32_t>(volBuf[s] * curVol);
                        volBuf[s] = static_cast<int16_t>(std::max(-32768, std::min(32767, scaled)));
                    }
                }

                // Đảm bảo AAudioStream đã thực sự bắt đầu phát khi có buffer đầu tiên sẵn sàng
                aaudio_stream_state_t st = AAudioStream_getState(stream_);
                if (st != AAUDIO_STREAM_STATE_STARTED && st != AAUDIO_STREAM_STATE_STARTING) {
                    AAudioStream_requestStart(stream_);
                }

                while (framesRemaining > 0 && isRunning_.load() && !isDestroying_.load() && isPlaying_.load()) {
                    int64_t timeoutNs = 50000000LL; // 50ms
                    aaudio_result_t written = AAudioStream_write(stream_, pcm, framesRemaining, timeoutNs);
                    if (written > 0) {
                        framesRemaining -= written;
                        pcm += written * pcmBytesPerFrame_;
                    } else if (written == AAUDIO_ERROR_TIMEOUT) {
                        continue;
                    } else {
                        LOGW("SourceVoiceInstance: AAudioStream_write error: %d (%s)", written, AAudio_convertResultToText(written));
                        break;
                    }
                }
            } else {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, task.durationUs, [this] {
                    return !isRunning_.load() || isDestroying_.load();
                });
            }

            if (!isRunning_.load() || isDestroying_.load()) break;

            // Gọi OnBufferEnd để cộng dồn master byte count và kích hoạt SetEvent
            callOnBufferEnd(task.pContext);

            samplesPlayed_ += (initialFrames > 0) ? initialFrames : (task.audioBytes / 4);
            if (buffersQueued_ > 0) buffersQueued_--;
            currentContext_ = nullptr;

            int16_t maxSample = 0;
            if ((formatTag_ == 2 || formatTag_ == 0x0161) && !task.decodedPcm.empty()) {
                for (int16_t s : task.decodedPcm) {
                    int16_t absVal = std::abs(s);
                    if (absVal > maxSample) maxSample = absVal;
                }
            } else if (task.pAudioData && task.audioBytes >= 2) {
                const auto* samples = reinterpret_cast<const int16_t*>(task.pAudioData);
                size_t numSamples = task.audioBytes / 2;
                for (size_t i = 0; i < numSamples; ++i) {
                    int16_t absVal = std::abs(samples[i]);
                    if (absVal > maxSample) maxSample = absVal;
                }
            }

            if (++sBufferCount % 500 == 1) {
                LOGI("SourceVoiceInstance: played %u buffers (bytes=%u, maxSample=%d, formatTag=0x%04X)",
                     sBufferCount, task.audioBytes, maxSample, formatTag_);
            }
        }
    }

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
        auto* inst = reinterpret_cast<SourceVoiceInstance*>(thisPtr);
        inst->setVolume(volume);
        return 0; // S_OK
    }

    static void WINAPI SourceVoice_GetVolume(void* thisPtr, float* pVolume) {
        if (pVolume) {
            auto* inst = reinterpret_cast<SourceVoiceInstance*>(thisPtr);
            *pVolume = inst->getVolume();
        }
    }

    static void WINAPI Voice_DestroyVoice(void* thisPtr) {
        LOGI("IXAudio2SourceVoice::DestroyVoice called: %p", thisPtr);
        delete reinterpret_cast<SourceVoiceInstance*>(thisPtr);
    }

    static int32_t WINAPI SourceVoice_Start(void* thisPtr, uint32_t flags, uint32_t opSet) {
        LOGI("IXAudio2SourceVoice::Start called: %p (flags=0x%X, opSet=%u)", thisPtr, flags, opSet);
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->start();
        return 0; // S_OK
    }

    static int32_t WINAPI SourceVoice_Stop(void* thisPtr, uint32_t flags, uint32_t opSet) {
        LOGI("IXAudio2SourceVoice::Stop called: %p (flags=0x%X, opSet=%u)", thisPtr, flags, opSet);
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->stop();
        return 0; // S_OK
    }

    static int32_t WINAPI SourceVoice_SubmitSourceBuffer(void* thisPtr, const void* pBuffer, const void* pBufferWMA) {
        auto* inst = reinterpret_cast<SourceVoiceInstance*>(thisPtr);
        auto* buf = reinterpret_cast<const XAUDIO2_BUFFER_64*>(pBuffer);
        LOGI("IXAudio2SourceVoice::SubmitSourceBuffer called: %p (bytes=%u, context=%p, pData=%p, pWma=%p)",
             thisPtr, buf ? buf->AudioBytes : 0, buf ? buf->pContext : nullptr, buf ? buf->pAudioData : nullptr, pBufferWMA);
        inst->submitBuffer(buf);
        return 0; // S_OK
    }

    static int32_t WINAPI SourceVoice_FlushSourceBuffers(void* thisPtr) {
        LOGI("IXAudio2SourceVoice::FlushSourceBuffers called: %p", thisPtr);
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->flush();
        return 0; // S_OK
    }

    static void WINAPI SourceVoice_GetState(void* thisPtr, void* pVoiceState, uint32_t) {
        auto* state = reinterpret_cast<XAUDIO2_VOICE_STATE_64*>(pVoiceState);
        reinterpret_cast<SourceVoiceInstance*>(thisPtr)->getState(state);
    }
};

// --- Mock Mastering Voice ---
static void* gMasteringVoiceVtable[32];
static void* gMockMasteringVoice = nullptr;

static void WINAPI MasteringVoice_GetVoiceDetails(void*, void* pDetails) {
    if (pDetails) {
        auto* details = reinterpret_cast<XAUDIO2_VOICE_DETAILS*>(pDetails);
        details->CreationFlags = 0;
        details->ActiveFlags = 0;
        details->InputChannels = 2;
        details->InputSampleRate = 48000;
        LOGI("IXAudio2MasteringVoice::GetVoiceDetails called -> ch=%u, rate=%u", details->InputChannels, details->InputSampleRate);
    }
}

static int32_t WINAPI MasteringVoice_SetVolume(void*, float volume, uint32_t) {
    LOGI("IXAudio2MasteringVoice::SetVolume -> %f", volume);
    return 0; // S_OK
}

static void WINAPI MasteringVoice_GetVolume(void*, float* pVolume) {
    if (pVolume) *pVolume = 1.0f;
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
    return 0; // S_OK
}
static uint32_t WINAPI xaudio_AddRef(void*) { return 1; }
static uint32_t WINAPI xaudio_Release(void*) { return 1; }

// Slot 5 (Offset 0x28): CreateSourceVoice
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
        if (formatTag != 1) { // Định dạng không phải PCM thô
            cbSize = *reinterpret_cast<const uint16_t*>(rawFmt + 16);
        }
    }
    LOGI("IXAudio2::CreateSourceVoice -> Format: tag=0x%04X (%u), ch=%u, rate=%u, avgBps=%u, align=%u, bits=%u, cbSize=%u",
         formatTag, formatTag, channels, sampleRate, avgBytesPerSec, blockAlign, bitsPerSample, cbSize);

    auto* voiceInst = new SourceVoiceInstance(pCallback, avgBytesPerSec, sampleRate, channels, bitsPerSample, formatTag, blockAlign, pSourceFormat);
    if (ppSourceVoice) {
        *ppSourceVoice = voiceInst;
    }
    return 0; // S_OK
}

// Slot 6 (Offset 0x30): CreateSubmixVoice
static int32_t WINAPI xaudio_CreateSubmixVoice(void*, void** ppSubmixVoice, uint32_t, uint32_t, uint32_t, uint32_t, const void*, const void*) {
    initSubmixVoiceVtable();
    if (ppSubmixVoice) *ppSubmixVoice = &gMockSubmixVoice;
    return 0; // S_OK
}

// Slot 7 (Offset 0x38): CreateMasteringVoice
static int32_t WINAPI xaudio_CreateMasteringVoice(void*, void** ppMasteringVoice, uint32_t, uint32_t, uint32_t, const void*, const void*, uint32_t) {
    initMasteringVoiceVtable();
    if (ppMasteringVoice) *ppMasteringVoice = &gMockMasteringVoice;
    return 0; // S_OK
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
    return 0; // S_OK
}

} // extern "C"
