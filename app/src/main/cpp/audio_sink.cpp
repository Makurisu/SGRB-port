#include "audio_sink.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>

#define LOG_TAG "SGRE_AudioSink"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" bool Win32Bridge_SetupCurrentThreadTEB();
extern "C" int32_t Win32Bridge_SetEvent(void* hEvent);

namespace {

// ============================================================================
// WaveOut Audio Sink (Xử lý âm thanh game miniaudio)
// ============================================================================
class WaveOutSink {
public:
    WaveOutSink() = default;
    ~WaveOutSink() {
        close();
    }

    uint32_t getDevCaps(void* pwoc, uint32_t cbwoc) {
        if (!pwoc || cbwoc == 0) return WINMM_MMSYSERR_INVALPARAM;

        WinWaveOutCapsA caps{};
        caps.wMid = 1; // MM_MICROSOFT
        caps.wPid = 1;
        caps.vDriverVersion = 0x0100;
        strncpy(caps.szPname, "Android AAudio Output", sizeof(caps.szPname) - 1);
        
        // Hỗ trợ đầy đủ các định dạng chuẩn: 48kHz, 44.1kHz, 22.05kHz, 11.025kHz 16-bit stereo
        // 0x000FFFFF bao gồm WAVE_FORMAT_48S16 (0x8000) và WAVE_FORMAT_44S16 (0x800) mà miniaudio kiểm tra
        caps.dwFormats = 0x000FFFFF;
        caps.wChannels = 2; // Stereo
        caps.wReserved1 = 0;
        caps.dwSupport = 0x0003; // WAVECAPS_VOLUME | WAVECAPS_LRVOLUME

        uint32_t copySize = std::min<uint32_t>(cbwoc, sizeof(WinWaveOutCapsA));
        memcpy(pwoc, &caps, copySize);
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t open(void** phwo, const void* pwfx, uintptr_t dwCallback, uintptr_t dwInstance, uint32_t fdwOpen) {
        std::lock_guard<std::mutex> lock(lifeMutex_);
        if (isOpen_) {
            close();
        }

        if (pwfx) {
            const auto* fmt = reinterpret_cast<const WinWaveFormatEx*>(pwfx);
            sampleRate_ = fmt->nSamplesPerSec > 0 ? fmt->nSamplesPerSec : 48000;
            channels_ = fmt->nChannels > 0 ? fmt->nChannels : 2;
            bitsPerSample_ = fmt->wBitsPerSample > 0 ? fmt->wBitsPerSample : 16;
            bytesPerFrame_ = channels_ * (bitsPerSample_ / 8);
            avgBytesPerSec_ = fmt->nAvgBytesPerSec > 0 ? fmt->nAvgBytesPerSec : (sampleRate_ * bytesPerFrame_);
        } else {
            sampleRate_ = 48000;
            channels_ = 2;
            bitsPerSample_ = 16;
            bytesPerFrame_ = 4;
            avgBytesPerSec_ = 48000 * 4;
        }

        dwCallback_ = dwCallback;
        dwInstance_ = dwInstance;
        fdwOpen_ = fdwOpen;
        callbackType_ = fdwOpen & WINMM_CALLBACK_TYPEMASK;
        hwo_ = reinterpret_cast<void*>(0x8001);

        if (phwo) {
            *phwo = hwo_;
        }

        LOGI("WaveOutSink::open -> Rate=%u, Channels=%u, Bits=%u, BPF=%u, CallbackType=0x%05X, Callback=%p",
             sampleRate_, channels_, bitsPerSample_, bytesPerFrame_, callbackType_, reinterpret_cast<void*>(dwCallback_));

        // Khởi tạo luồng xuất âm thanh AAudio
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t res = AAudio_createStreamBuilder(&builder);
        if (res == AAUDIO_OK && builder) {
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSampleRate(builder, sampleRate_);
            AAudioStreamBuilder_setChannelCount(builder, channels_);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

            res = AAudioStreamBuilder_openStream(builder, &stream_);
            AAudioStreamBuilder_delete(builder);

            if (res == AAUDIO_OK && stream_) {
                res = AAudioStream_requestStart(stream_);
                LOGI("AAudioStream opened and started successfully: Stream=%p, SampleRate=%d, Channels=%d",
                     stream_, AAudioStream_getSampleRate(stream_), AAudioStream_getChannelCount(stream_));
            } else {
                LOGW("AAudioStreamBuilder_openStream failed: %d (%s), falling back to clock emulation",
                     res, AAudio_convertResultToText(res));
                stream_ = nullptr;
            }
        } else {
            LOGW("AAudio_createStreamBuilder failed: %d, falling back to clock emulation", res);
            stream_ = nullptr;
        }

        isOpen_ = true;
        isRunning_ = true;
        isPaused_ = false;
        isResetting_ = false;
        totalFramesPlayed_ = 0;

        // Bắt đầu luồng worker xử lý phát buffer
        workerThread_ = std::thread(&WaveOutSink::workerLoop, this);

        // Gọi callback WOM_OPEN nếu là Function Callback
        if (callbackType_ == WINMM_CALLBACK_FUNCTION && dwCallback_) {
            using WaveOutProc = void (WINAPI *)(void*, uint32_t, uintptr_t, uintptr_t, uintptr_t);
            auto fn = reinterpret_cast<WaveOutProc>(dwCallback_);
            fn(hwo_, WINMM_WOM_OPEN, dwInstance_, 0, 0);
        }

        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t close() {
        std::lock_guard<std::mutex> lock(lifeMutex_);
        if (!isOpen_) return WINMM_MMSYSERR_NOERROR;

        LOGI("WaveOutSink::close called");
        isRunning_ = false;
        isResetting_ = true;
        cv_.notify_all();

        if (workerThread_.joinable()) {
            workerThread_.join();
        }

        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }

        // Dọn sạch hàng đợi và hoàn tất các buffer còn dở dang
        {
            std::lock_guard<std::mutex> qLock(queueMutex_);
            while (!bufferQueue_.empty()) {
                auto* hdr = bufferQueue_.front();
                bufferQueue_.pop();
                hdr->dwFlags &= ~WINMM_WHDR_INQUEUE;
                hdr->dwFlags |= WINMM_WHDR_DONE;
                notifyBufferDone(hdr);
            }
        }

        if (callbackType_ == WINMM_CALLBACK_FUNCTION && dwCallback_) {
            using WaveOutProc = void (WINAPI *)(void*, uint32_t, uintptr_t, uintptr_t, uintptr_t);
            auto fn = reinterpret_cast<WaveOutProc>(dwCallback_);
            fn(hwo_, WINMM_WOM_CLOSE, dwInstance_, 0, 0);
        }

        isOpen_ = false;
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t write(void* pwh) {
        if (!pwh) return WINMM_MMSYSERR_INVALPARAM;
        auto* hdr = reinterpret_cast<WinWaveHdr*>(pwh);
        if (!hdr->lpData || hdr->dwBufferLength == 0) return WINMM_MMSYSERR_INVALPARAM;

        // Đánh dấu buffer đang trong hàng đợi phát
        hdr->dwFlags |= WINMM_WHDR_INQUEUE;
        hdr->dwFlags &= ~WINMM_WHDR_DONE;

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            bufferQueue_.push(hdr);
        }
        cv_.notify_one();
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t reset() {
        LOGI("WaveOutSink::reset called");
        isResetting_ = true;
        cv_.notify_all();

        if (stream_) {
            AAudioStream_requestFlush(stream_);
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!bufferQueue_.empty()) {
                auto* hdr = bufferQueue_.front();
                bufferQueue_.pop();
                hdr->dwFlags &= ~WINMM_WHDR_INQUEUE;
                hdr->dwFlags |= WINMM_WHDR_DONE;
                notifyBufferDone(hdr);
            }
        }
        isResetting_ = false;
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t pause() {
        LOGI("WaveOutSink::pause called");
        isPaused_ = true;
        if (stream_) {
            AAudioStream_requestPause(stream_);
        }
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t restart() {
        LOGI("WaveOutSink::restart called");
        isPaused_ = false;
        if (stream_) {
            AAudioStream_requestStart(stream_);
        }
        cv_.notify_all();
        return WINMM_MMSYSERR_NOERROR;
    }

    uint32_t getPosition(void* pmmt, uint32_t cbmmt) {
        if (!pmmt || cbmmt < sizeof(uint32_t)) return WINMM_MMSYSERR_INVALPARAM;
        auto* t = reinterpret_cast<WinMmTime*>(pmmt);
        uint64_t frames = totalFramesPlayed_.load();
        uint64_t bytes = frames * bytesPerFrame_;

        if (t->wType == WINMM_TIME_MS) {
            t->u.ms = avgBytesPerSec_ > 0 ? static_cast<uint32_t>((bytes * 1000ULL) / avgBytesPerSec_) : 0;
        } else if (t->wType == WINMM_TIME_SAMPLES) {
            t->u.sample = static_cast<uint32_t>(frames);
        } else {
            t->wType = WINMM_TIME_BYTES;
            t->u.cb = static_cast<uint32_t>(bytes);
        }
        return WINMM_MMSYSERR_NOERROR;
    }

private:
    std::mutex lifeMutex_;
    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::queue<WinWaveHdr*> bufferQueue_;
    std::thread workerThread_;

    AAudioStream* stream_ = nullptr;
    std::atomic<bool> isOpen_{ false };
    std::atomic<bool> isRunning_{ false };
    std::atomic<bool> isPaused_{ false };
    std::atomic<bool> isResetting_{ false };
    std::atomic<uint64_t> totalFramesPlayed_{ 0 };

    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t bitsPerSample_ = 16;
    uint32_t bytesPerFrame_ = 4;
    uint32_t avgBytesPerSec_ = 192000;

    void* hwo_ = nullptr;
    uintptr_t dwCallback_ = 0;
    uintptr_t dwInstance_ = 0;
    uint32_t fdwOpen_ = 0;
    uint32_t callbackType_ = 0;

    void notifyBufferDone(WinWaveHdr* hdr) {
        if (!hdr) return;
        if (callbackType_ == WINMM_CALLBACK_EVENT && dwCallback_) {
            Win32Bridge_SetEvent(reinterpret_cast<void*>(dwCallback_));
        } else if (callbackType_ == WINMM_CALLBACK_FUNCTION && dwCallback_) {
            using WaveOutProc = void (WINAPI *)(void*, uint32_t, uintptr_t, uintptr_t, uintptr_t);
            auto fn = reinterpret_cast<WaveOutProc>(dwCallback_);
            fn(hwo_, WINMM_WOM_DONE, dwInstance_, reinterpret_cast<uintptr_t>(hdr), 0);
        }
    }

    void workerLoop() {
        Win32Bridge_SetupCurrentThreadTEB();
        static uint32_t sBuffersPlayed = 0;

        while (isRunning_.load()) {
            WinWaveHdr* hdr = nullptr;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                cv_.wait(lock, [this] {
                    return !isRunning_.load() || isResetting_.load() || (!isPaused_.load() && !bufferQueue_.empty());
                });

                if (!isRunning_.load()) break;

                if (isResetting_.load()) {
                    while (!bufferQueue_.empty()) {
                        auto* pending = bufferQueue_.front();
                        bufferQueue_.pop();
                        pending->dwFlags &= ~WINMM_WHDR_INQUEUE;
                        pending->dwFlags |= WINMM_WHDR_DONE;
                        notifyBufferDone(pending);
                    }
                    continue;
                }

                if (isPaused_.load() || bufferQueue_.empty()) continue;

                hdr = bufferQueue_.front();
                bufferQueue_.pop();
            }

            if (!hdr) continue;

            uint32_t bufferBytes = hdr->dwBufferLength;
            uint32_t frameCount = bytesPerFrame_ > 0 ? (bufferBytes / bytesPerFrame_) : 0;
            const uint8_t* pcmData = reinterpret_cast<const uint8_t*>(hdr->lpData);

            // Kiểm tra biên độ âm thanh thực tế trong buffer PCM 16-bit
            int16_t maxSample = 0;
            if (pcmData && bufferBytes >= 2) {
                const auto* samples = reinterpret_cast<const int16_t*>(pcmData);
                size_t numSamples = bufferBytes / 2;
                for (size_t i = 0; i < numSamples; ++i) {
                    int16_t s = std::abs(samples[i]);
                    if (s > maxSample) maxSample = s;
                }
            }
            static uint32_t sNonSilentBuffers = 0;
            if (maxSample > 0) {
                sNonSilentBuffers++;
            }

            if (stream_ && frameCount > 0) {
                int32_t framesRemaining = static_cast<int32_t>(frameCount);
                while (framesRemaining > 0 && isRunning_.load() && !isResetting_.load() && !isPaused_.load()) {
                    // Ghi buffer vào phần cứng AAudio với timeout 50ms
                    int64_t timeoutNs = 50000000LL;
                    aaudio_result_t written = AAudioStream_write(stream_, pcmData, framesRemaining, timeoutNs);
                    if (written > 0) {
                        framesRemaining -= written;
                        pcmData += written * bytesPerFrame_;
                        totalFramesPlayed_ += written;
                    } else if (written == AAUDIO_ERROR_TIMEOUT) {
                        continue;
                    } else {
                        LOGW("AAudioStream_write error: %d (%s), advancing buffer to prevent deadlock",
                             written, AAudio_convertResultToText(written));
                        break;
                    }
                }
            } else if (frameCount > 0) {
                // Giả lập đồng hồ phát âm thanh khi stream phần cứng không mở được
                uint64_t durationUs = (static_cast<uint64_t>(bufferBytes) * 1000000ULL) / avgBytesPerSec_;
                std::this_thread::sleep_for(std::chrono::microseconds(durationUs));
                totalFramesPlayed_ += frameCount;
            }

            // Hoàn tất buffer và kích hoạt callback giải phóng cho miniaudio
            hdr->dwFlags &= ~WINMM_WHDR_INQUEUE;
            hdr->dwFlags |= WINMM_WHDR_DONE;
            notifyBufferDone(hdr);

            if (++sBuffersPlayed % 500 == 1) {
                LOGI("WaveOutSink: played %u buffers (totalFrames=%llu, rate=%u, maxSample=%d, nonSilentBuffers=%u)",
                     sBuffersPlayed, static_cast<unsigned long long>(totalFramesPlayed_.load()), sampleRate_, maxSample, sNonSilentBuffers);
            }
        }
    }
};

static WaveOutSink gWaveOutSink;

// ============================================================================
// XAudio2 Audio Sink (Xử lý âm thanh Movie / Cutscene qua Media Foundation)
// ============================================================================
class XAudio2Sink {
public:
    XAudio2Sink() = default;
    ~XAudio2Sink() {
        close();
    }

    bool open(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isOpen_) {
            close();
        }

        sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        channels_ = channels > 0 ? channels : 2;
        bitsPerSample_ = bitsPerSample > 0 ? bitsPerSample : 16;
        bytesPerFrame_ = channels_ * (bitsPerSample_ / 8);

        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t res = AAudio_createStreamBuilder(&builder);
        if (res == AAUDIO_OK && builder) {
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setSampleRate(builder, sampleRate_);
            AAudioStreamBuilder_setChannelCount(builder, channels_);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

            res = AAudioStreamBuilder_openStream(builder, &stream_);
            AAudioStreamBuilder_delete(builder);

            if (res == AAUDIO_OK && stream_) {
                res = AAudioStream_requestStart(stream_);
                LOGI("XAudio2Sink: AAudio stream opened: Rate=%d, Channels=%d",
                     AAudioStream_getSampleRate(stream_), AAudioStream_getChannelCount(stream_));
                isOpen_ = true;
                return true;
            } else {
                LOGW("XAudio2Sink: openStream failed: %d (%s)", res, AAudio_convertResultToText(res));
                stream_ = nullptr;
            }
        }
        return false;
    }

    void write(const void* data, size_t bytes) {
        if (!data || bytes == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isOpen_ || !stream_ || bytesPerFrame_ == 0) return;

        int32_t framesRemaining = static_cast<int32_t>(bytes / bytesPerFrame_);
        const uint8_t* pcm = reinterpret_cast<const uint8_t*>(data);

        while (framesRemaining > 0 && isOpen_) {
            int64_t timeoutNs = 50000000LL; // 50ms
            aaudio_result_t written = AAudioStream_write(stream_, pcm, framesRemaining, timeoutNs);
            if (written > 0) {
                framesRemaining -= written;
                pcm += written * bytesPerFrame_;
            } else if (written == AAUDIO_ERROR_TIMEOUT) {
                continue;
            } else {
                LOGW("XAudio2Sink: AAudioStream_write error: %d (%s)", written, AAudio_convertResultToText(written));
                break;
            }
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_) {
            AAudioStream_requestFlush(stream_);
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isOpen_) return;
        LOGI("XAudio2Sink::close called");
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        isOpen_ = false;
    }

    bool isOpen() const {
        return isOpen_.load();
    }

private:
    std::mutex mutex_;
    AAudioStream* stream_ = nullptr;
    std::atomic<bool> isOpen_{ false };
    uint32_t sampleRate_ = 48000;
    uint32_t channels_ = 2;
    uint32_t bitsPerSample_ = 16;
    uint32_t bytesPerFrame_ = 4;
};

static XAudio2Sink gXAudio2Sink;

} // anonymous namespace

// ============================================================================
// Xuất các hàm C API cho WaveOut & XAudio2
// ============================================================================
extern "C" {

uint32_t AudioSink_WaveOutGetNumDevs() {
    return 1;
}

uint32_t AudioSink_WaveOutGetDevCapsA(uintptr_t, void* pwoc, uint32_t cbwoc) {
    return gWaveOutSink.getDevCaps(pwoc, cbwoc);
}

uint32_t AudioSink_WaveOutOpen(void** phwo, uint32_t, const void* pwfx, uintptr_t dwCallback, uintptr_t dwInstance, uint32_t fdwOpen) {
    return gWaveOutSink.open(phwo, pwfx, dwCallback, dwInstance, fdwOpen);
}

uint32_t AudioSink_WaveOutClose(void*) {
    return gWaveOutSink.close();
}

uint32_t AudioSink_WaveOutPrepareHeader(void*, void* pwh, uint32_t) {
    if (pwh) {
        auto* hdr = reinterpret_cast<WinWaveHdr*>(pwh);
        hdr->dwFlags |= WINMM_WHDR_PREPARED;
    }
    return WINMM_MMSYSERR_NOERROR;
}

uint32_t AudioSink_WaveOutUnprepareHeader(void*, void* pwh, uint32_t) {
    if (pwh) {
        auto* hdr = reinterpret_cast<WinWaveHdr*>(pwh);
        hdr->dwFlags &= ~WINMM_WHDR_PREPARED;
    }
    return WINMM_MMSYSERR_NOERROR;
}

uint32_t AudioSink_WaveOutWrite(void*, void* pwh, uint32_t) {
    return gWaveOutSink.write(pwh);
}

uint32_t AudioSink_WaveOutReset(void*) {
    return gWaveOutSink.reset();
}

uint32_t AudioSink_WaveOutPause(void*) {
    return gWaveOutSink.pause();
}

uint32_t AudioSink_WaveOutRestart(void*) {
    return gWaveOutSink.restart();
}

uint32_t AudioSink_WaveOutGetPosition(void*, void* pmmt, uint32_t cbmmt) {
    return gWaveOutSink.getPosition(pmmt, cbmmt);
}

bool AudioSink_XAudio2Open(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample) {
    return gXAudio2Sink.open(sampleRate, channels, bitsPerSample);
}

void AudioSink_XAudio2Write(const void* data, size_t bytes) {
    gXAudio2Sink.write(data, bytes);
}

void AudioSink_XAudio2Flush() {
    gXAudio2Sink.flush();
}

void AudioSink_XAudio2Close() {
    gXAudio2Sink.close();
}

bool AudioSink_XAudio2IsOpen() {
    return gXAudio2Sink.isOpen();
}

} // extern "C"
