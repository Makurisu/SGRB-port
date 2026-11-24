#include "webm_decoder.h"
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define LOG_TAG "SGRE_WebmDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

extern "C" void Win32Bridge_SetupCurrentThreadTEB();

namespace sgre {

namespace {

// Khai báo kiểu cho AMediaDataSource (Android API 28+)
typedef struct AMediaDataSource AMediaDataSource;
typedef ssize_t (*AMediaDataSourceReadAt)(void *userdata, off64_t offset, void * buffer, size_t size);
typedef off64_t (*AMediaDataSourceGetSize)(void *userdata);
typedef void (*AMediaDataSourceClose)(void *userdata);

typedef AMediaDataSource* (*fn_AMediaDataSource_new)();
typedef AMediaDataSource* (*fn_AMediaDataSource_newUri)(const char *uri, int numheaders, const char * const *key_values);
typedef void (*fn_AMediaDataSource_delete)(AMediaDataSource *);
typedef void (*fn_AMediaDataSource_setUserdata)(AMediaDataSource *, void *userdata);
typedef void (*fn_AMediaDataSource_setReadAt)(AMediaDataSource *, AMediaDataSourceReadAt);
typedef void (*fn_AMediaDataSource_setGetSize)(AMediaDataSource *, AMediaDataSourceGetSize);
typedef void (*fn_AMediaDataSource_setClose)(AMediaDataSource *, AMediaDataSourceClose);
typedef media_status_t (*fn_AMediaExtractor_setDataSourceCustom)(AMediaExtractor*, AMediaDataSource *);

struct NdkDataSourceApi {
    fn_AMediaDataSource_new dataSourceNew = nullptr;
    fn_AMediaDataSource_delete dataSourceDelete = nullptr;
    fn_AMediaDataSource_setUserdata dataSourceSetUserdata = nullptr;
    fn_AMediaDataSource_setReadAt dataSourceSetReadAt = nullptr;
    fn_AMediaDataSource_setGetSize dataSourceSetGetSize = nullptr;
    fn_AMediaDataSource_setClose dataSourceSetClose = nullptr;
    fn_AMediaExtractor_setDataSourceCustom extractorSetDataSourceCustom = nullptr;
    bool isAvailable = false;
};

static NdkDataSourceApi gNdkApi;

static void initNdkApiOnce() {
    static bool sInitialized = false;
    if (sInitialized) return;
    sInitialized = true;

    void* handle = dlopen("libmediandk.so", RTLD_NOW);
    if (!handle) return;

    gNdkApi.dataSourceNew = reinterpret_cast<fn_AMediaDataSource_new>(dlsym(handle, "AMediaDataSource_new"));
    gNdkApi.dataSourceDelete = reinterpret_cast<fn_AMediaDataSource_delete>(dlsym(handle, "AMediaDataSource_delete"));
    gNdkApi.dataSourceSetUserdata = reinterpret_cast<fn_AMediaDataSource_setUserdata>(dlsym(handle, "AMediaDataSource_setUserdata"));
    gNdkApi.dataSourceSetReadAt = reinterpret_cast<fn_AMediaDataSource_setReadAt>(dlsym(handle, "AMediaDataSource_setReadAt"));
    gNdkApi.dataSourceSetGetSize = reinterpret_cast<fn_AMediaDataSource_setGetSize>(dlsym(handle, "AMediaDataSource_setGetSize"));
    gNdkApi.dataSourceSetClose = reinterpret_cast<fn_AMediaDataSource_setClose>(dlsym(handle, "AMediaDataSource_setClose"));
    gNdkApi.extractorSetDataSourceCustom = reinterpret_cast<fn_AMediaExtractor_setDataSourceCustom>(dlsym(handle, "AMediaExtractor_setDataSourceCustom"));

    gNdkApi.isAvailable = (gNdkApi.dataSourceNew && gNdkApi.dataSourceDelete &&
                           gNdkApi.dataSourceSetReadAt && gNdkApi.dataSourceSetGetSize &&
                           gNdkApi.extractorSetDataSourceCustom);
    LOGI("NdkDataSourceApi initialized: available=%d", gNdkApi.isAvailable ? 1 : 0);
}

// Bố cục vtable của MovieReadByteStream trong sgre_steam.exe
// Slot 0..2: IUnknown
// Slot 4 (offset 32 = 0x20): GetLength(QWORD* pLength)
// Slot 7 (offset 56 = 0x38): SetCurrentPosition(QWORD position)
// Slot 9 (offset 72 = 0x48): Read(void* pb, ULONG cb, ULONG* pcbRead)
using FnGetLength = int32_t (WINAPI *)(void* thisPtr, uint64_t* pLength);
using FnSetPos = int32_t (WINAPI *)(void* thisPtr, uint64_t position);
using FnRead = int32_t (WINAPI *)(void* thisPtr, void* pb, uint32_t cb, uint32_t* pcbRead);

struct ByteStreamReader {
    void* pByteStream = nullptr;
    std::mutex mutex;
    uint64_t totalLength = 0;

    int32_t getLength(uint64_t* outLen) {
        if (!pByteStream || !outLen) return -1;
        void** vtable = *reinterpret_cast<void***>(pByteStream);
        auto fn = reinterpret_cast<FnGetLength>(vtable[4]);
        return fn(pByteStream, outLen);
    }

    int32_t setPosition(uint64_t pos) {
        if (!pByteStream) return -1;
        void** vtable = *reinterpret_cast<void***>(pByteStream);
        auto fn = reinterpret_cast<FnSetPos>(vtable[7]);
        return fn(pByteStream, pos);
    }

    int32_t read(void* buf, uint32_t size, uint32_t* bytesRead) {
        if (!pByteStream || !buf || !bytesRead) return -1;
        void** vtable = *reinterpret_cast<void***>(pByteStream);
        auto fn = reinterpret_cast<FnRead>(vtable[9]);
        return fn(pByteStream, buf, size, bytesRead);
    }

    ssize_t readAt(off64_t offset, void* buffer, size_t size) {
        Win32Bridge_SetupCurrentThreadTEB();
        std::lock_guard<std::mutex> lock(mutex);
        if (offset < 0 || static_cast<uint64_t>(offset) >= totalLength) {
            return 0;
        }
        if (setPosition(static_cast<uint64_t>(offset)) < 0) {
            return -1;
        }
        uint32_t actualRead = 0;
        uint32_t toRead = static_cast<uint32_t>(std::min<size_t>(size, totalLength - offset));
        if (read(buffer, toRead, &actualRead) < 0) {
            return -1;
        }
        return static_cast<ssize_t>(actualRead);
    }
};

static ssize_t onMediaDataSourceReadAt(void *userdata, off64_t offset, void * buffer, size_t size) {
    auto* reader = reinterpret_cast<ByteStreamReader*>(userdata);
    if (!reader) return -1;
    return reader->readAt(offset, buffer, size);
}

static off64_t onMediaDataSourceGetSize(void *userdata) {
    auto* reader = reinterpret_cast<ByteStreamReader*>(userdata);
    if (!reader) return 0;
    return static_cast<off64_t>(reader->totalLength);
}

static void onMediaDataSourceClose(void*) {}

} // namespace

struct WebmDecoder::Impl {
    ByteStreamReader reader;
    AMediaDataSource* ndkDataSource = nullptr;
    AMediaExtractor* extractor = nullptr;
    int memFd = -1;

    // Video
    int videoTrackIdx = -1;
    AMediaCodec* videoCodec = nullptr;
    bool videoEos = false;
    uint32_t videoColorFormat = 0;
    uint32_t videoStride = 0;
    uint32_t videoSliceHeight = 0;

    // Audio
    int audioTrackIdx = -1;
    AMediaCodec* audioCodec = nullptr;
    bool audioEos = false;

    ~Impl() {
        cleanup();
    }

    void cleanup() {
        if (videoCodec) {
            AMediaCodec_stop(videoCodec);
            AMediaCodec_delete(videoCodec);
            videoCodec = nullptr;
        }
        if (audioCodec) {
            AMediaCodec_stop(audioCodec);
            AMediaCodec_delete(audioCodec);
            audioCodec = nullptr;
        }
        if (extractor) {
            AMediaExtractor_delete(extractor);
            extractor = nullptr;
        }
        if (ndkDataSource && gNdkApi.dataSourceDelete) {
            gNdkApi.dataSourceDelete(ndkDataSource);
            ndkDataSource = nullptr;
        }
        if (memFd >= 0) {
            ::close(memFd);
            memFd = -1;
        }
    }
};

WebmDecoder::WebmDecoder() : impl_(std::make_unique<Impl>()) {
    initNdkApiOnce();
}

WebmDecoder::~WebmDecoder() {
    close();
}

void WebmDecoder::close() {
    if (impl_) {
        impl_->cleanup();
    }
    hasVideo_ = false;
    hasAudio_ = false;
    videoWidth_ = 0;
    videoHeight_ = 0;
    duration100ns_ = 0;
}

bool WebmDecoder::initFromByteStream(void* pByteStream, const std::string& nameHint) {
    if (!pByteStream) return false;
    close();

    impl_->reader.pByteStream = pByteStream;
    uint64_t totalLen = 0;
    if (impl_->reader.getLength(&totalLen) < 0 || totalLen == 0) {
        LOGW("initFromByteStream: ByteStream empty or GetLength failed (hint: %s)", nameHint.c_str());
        return false;
    }
    impl_->reader.totalLength = totalLen;
    LOGI("initFromByteStream: hint='%s', totalLength=%llu bytes", nameHint.c_str(), static_cast<unsigned long long>(totalLen));

    impl_->extractor = AMediaExtractor_new();
    if (!impl_->extractor) {
        LOGE("AMediaExtractor_new failed");
        return false;
    }

    bool dataSourceSet = false;
    if (gNdkApi.isAvailable) {
        impl_->ndkDataSource = gNdkApi.dataSourceNew();
        if (impl_->ndkDataSource) {
            gNdkApi.dataSourceSetUserdata(impl_->ndkDataSource, &impl_->reader);
            gNdkApi.dataSourceSetReadAt(impl_->ndkDataSource, onMediaDataSourceReadAt);
            gNdkApi.dataSourceSetGetSize(impl_->ndkDataSource, onMediaDataSourceGetSize);
            gNdkApi.dataSourceSetClose(impl_->ndkDataSource, onMediaDataSourceClose);
            media_status_t status = gNdkApi.extractorSetDataSourceCustom(impl_->extractor, impl_->ndkDataSource);
            if (status == AMEDIA_OK) {
                dataSourceSet = true;
                LOGI("AMediaExtractor set custom data source SUCCESS");
            } else {
                LOGW("AMediaExtractor set custom data source FAILED: %d", status);
            }
        }
    }

    // Fallback nếu API 28+ không hỗ trợ hoặc setDataSourceCustom thất bại:
    // Dùng memfd_create trong RAM để cấp fd cho AMediaExtractor
    if (!dataSourceSet) {
#if defined(__NR_memfd_create)
        impl_->memFd = static_cast<int>(syscall(__NR_memfd_create, "webm_mdf", 1 /* MFD_CLOEXEC */));
#else
        impl_->memFd = -1;
#endif
        if (impl_->memFd >= 0) {
            LOGI("memfd_create SUCCESS, dumping %llu bytes from ByteStream...", static_cast<unsigned long long>(totalLen));
            constexpr size_t kChunkSize = 256 * 1024;
            std::vector<uint8_t> chunk(kChunkSize);
            uint64_t offset = 0;
            while (offset < totalLen) {
                size_t toRead = static_cast<size_t>(std::min<uint64_t>(kChunkSize, totalLen - offset));
                ssize_t rd = impl_->reader.readAt(static_cast<off64_t>(offset), chunk.data(), toRead);
                if (rd <= 0) break;
                ssize_t wr = ::write(impl_->memFd, chunk.data(), static_cast<size_t>(rd));
                if (wr != rd) break;
                offset += static_cast<uint64_t>(rd);
            }
            if (offset == totalLen) {
                media_status_t st = AMediaExtractor_setDataSourceFd(impl_->extractor, impl_->memFd, 0, static_cast<off64_t>(totalLen));
                if (st == AMEDIA_OK) {
                    dataSourceSet = true;
                    LOGI("AMediaExtractor_setDataSourceFd with memfd SUCCESS");
                } else {
                    LOGW("AMediaExtractor_setDataSourceFd failed: %d", st);
                }
            }
        }
    }

    if (!dataSourceSet) {
        LOGE("Failed to set data source for AMediaExtractor");
        close();
        return false;
    }

    // Quét tìm track Video và Audio
    size_t numTracks = AMediaExtractor_getTrackCount(impl_->extractor);
    LOGI("AMediaExtractor track count: %zu", numTracks);

    for (size_t i = 0; i < numTracks; ++i) {
        AMediaFormat* format = AMediaExtractor_getTrackFormat(impl_->extractor, i);
        if (!format) continue;

        const char* mime = nullptr;
        if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime) {
            int64_t durationUs = 0;
            AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationUs);
            if (durationUs > 0) {
                duration100ns_ = std::max(duration100ns_, durationUs * 10);
            }

            if (strncmp(mime, "video/", 6) == 0 && impl_->videoTrackIdx < 0) {
                impl_->videoTrackIdx = static_cast<int>(i);
                int32_t w = 0, h = 0;
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h);
                videoWidth_ = static_cast<uint32_t>(w);
                videoHeight_ = static_cast<uint32_t>(h);

                impl_->videoCodec = AMediaCodec_createDecoderByType(mime);
                if (impl_->videoCodec) {
                    media_status_t st = AMediaCodec_configure(impl_->videoCodec, format, nullptr, nullptr, 0);
                    if (st == AMEDIA_OK && AMediaCodec_start(impl_->videoCodec) == AMEDIA_OK) {
                        AMediaExtractor_selectTrack(impl_->extractor, i);
                        hasVideo_ = true;
                        LOGI("Video track [%zu]: mime='%s', %ux%u, duration=%.2fs",
                             i, mime, videoWidth_, videoHeight_, static_cast<double>(durationUs) / 1000000.0);
                    } else {
                        LOGE("Failed to configure/start video codec for mime '%s'", mime);
                        AMediaCodec_delete(impl_->videoCodec);
                        impl_->videoCodec = nullptr;
                    }
                }
            } else if (strncmp(mime, "audio/", 6) == 0 && impl_->audioTrackIdx < 0) {
                impl_->audioTrackIdx = static_cast<int>(i);
                int32_t sr = 0, ch = 0;
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
                audioSampleRate_ = sr > 0 ? static_cast<uint32_t>(sr) : 48000;
                audioChannels_ = ch > 0 ? static_cast<uint32_t>(ch) : 2;

                impl_->audioCodec = AMediaCodec_createDecoderByType(mime);
                if (impl_->audioCodec) {
                    media_status_t st = AMediaCodec_configure(impl_->audioCodec, format, nullptr, nullptr, 0);
                    if (st == AMEDIA_OK && AMediaCodec_start(impl_->audioCodec) == AMEDIA_OK) {
                        AMediaExtractor_selectTrack(impl_->extractor, i);
                        hasAudio_ = true;
                        LOGI("Audio track [%zu]: mime='%s', sr=%u, ch=%u", i, mime, audioSampleRate_, audioChannels_);
                    } else {
                        LOGE("Failed to configure/start audio codec for mime '%s'", mime);
                        AMediaCodec_delete(impl_->audioCodec);
                        impl_->audioCodec = nullptr;
                    }
                }
            }
        }
        AMediaFormat_delete(format);
    }

    if (!hasVideo_ && !hasAudio_) {
        LOGE("No decodable video or audio track found");
        close();
        return false;
    }

    return true;
}

void WebmDecoder::seekTo(int64_t ptsUs) {
    if (!impl_ || !impl_->extractor) return;
    AMediaExtractor_seekTo(impl_->extractor, ptsUs, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
    if (impl_->videoCodec) {
        AMediaCodec_flush(impl_->videoCodec);
        impl_->videoEos = false;
    }
    if (impl_->audioCodec) {
        AMediaCodec_flush(impl_->audioCodec);
        impl_->audioEos = false;
    }
}

bool WebmDecoder::readNextVideoFrame(DecodedVideoFrame& outFrame) {
    if (!hasVideo_ || !impl_ || !impl_->videoCodec) return false;

    constexpr int64_t kTimeoutUs = 5000; // 5ms timeout
    int maxAttempts = 50;

    while (maxAttempts-- > 0) {
        // 1. Cung cấp input buffer cho codec nếu chưa EOS
        if (!impl_->videoEos) {
            ssize_t inIdx = AMediaCodec_dequeueInputBuffer(impl_->videoCodec, 0);
            if (inIdx >= 0) {
                size_t inSize = 0;
                uint8_t* inBuf = AMediaCodec_getInputBuffer(impl_->videoCodec, static_cast<size_t>(inIdx), &inSize);
                if (inBuf && inSize > 0) {
                    int trackIdx = AMediaExtractor_getSampleTrackIndex(impl_->extractor);
                    if (trackIdx == impl_->videoTrackIdx) {
                        ssize_t sampleSize = AMediaExtractor_readSampleData(impl_->extractor, inBuf, inSize);
                        if (sampleSize >= 0) {
                            int64_t pts = AMediaExtractor_getSampleTime(impl_->extractor);
                            AMediaCodec_queueInputBuffer(impl_->videoCodec, static_cast<size_t>(inIdx), 0,
                                                         static_cast<size_t>(sampleSize), pts, 0);
                            AMediaExtractor_advance(impl_->extractor);
                        } else {
                            AMediaCodec_queueInputBuffer(impl_->videoCodec, static_cast<size_t>(inIdx), 0, 0, 0,
                                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                            impl_->videoEos = true;
                        }
                    } else if (trackIdx < 0) {
                        AMediaCodec_queueInputBuffer(impl_->videoCodec, static_cast<size_t>(inIdx), 0, 0, 0,
                                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        impl_->videoEos = true;
                    } else {
                        // Track hiện tại là Audio, bỏ qua buffer video này để advance audio
                        AMediaCodec_queueInputBuffer(impl_->videoCodec, static_cast<size_t>(inIdx), 0, 0, 0, 0);
                    }
                }
            }
        }

        // 2. Lấy output buffer đã giải mã
        AMediaCodecBufferInfo info{};
        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(impl_->videoCodec, &info, kTimeoutUs);
        if (outIdx >= 0) {
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                AMediaCodec_releaseOutputBuffer(impl_->videoCodec, static_cast<size_t>(outIdx), false);
                return false;
            }

            size_t outSize = 0;
            uint8_t* outBuf = AMediaCodec_getOutputBuffer(impl_->videoCodec, static_cast<size_t>(outIdx), &outSize);
            if (outBuf && info.size > 0) {
                outFrame.pixelData.assign(outBuf + info.offset, outBuf + info.offset + info.size);
                outFrame.ptsUs = info.presentationTimeUs;
                outFrame.durationUs = 16667; // Mặc định 60fps
                outFrame.width = videoWidth_;
                outFrame.height = videoHeight_;
                outFrame.colorFormat = impl_->videoColorFormat;
                outFrame.stride = impl_->videoStride > 0 ? impl_->videoStride : videoWidth_;
                outFrame.sliceHeight = impl_->videoSliceHeight > 0 ? impl_->videoSliceHeight : videoHeight_;

                AMediaCodec_releaseOutputBuffer(impl_->videoCodec, static_cast<size_t>(outIdx), false);
                return true;
            }
            AMediaCodec_releaseOutputBuffer(impl_->videoCodec, static_cast<size_t>(outIdx), false);
        } else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(impl_->videoCodec);
            if (format) {
                int32_t val = 0;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &val) && val > 0) videoWidth_ = val;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &val) && val > 0) videoHeight_ = val;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &val) && val > 0) impl_->videoStride = val;
                if (AMediaFormat_getInt32(format, "slice-height", &val) && val > 0) impl_->videoSliceHeight = val;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &val)) impl_->videoColorFormat = val;
                LOGI("Video format changed: %ux%u, stride=%u, sliceH=%u, colorFmt=0x%x",
                     videoWidth_, videoHeight_, impl_->videoStride, impl_->videoSliceHeight, impl_->videoColorFormat);
                AMediaFormat_delete(format);
            }
        } else if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (impl_->videoEos) return false;
        }
    }
    return false;
}

bool WebmDecoder::readNextAudioFrame(DecodedAudioFrame& outFrame) {
    if (!hasAudio_ || !impl_ || !impl_->audioCodec) return false;

    constexpr int64_t kTimeoutUs = 5000;
    int maxAttempts = 50;

    while (maxAttempts-- > 0) {
        if (!impl_->audioEos) {
            ssize_t inIdx = AMediaCodec_dequeueInputBuffer(impl_->audioCodec, 0);
            if (inIdx >= 0) {
                size_t inSize = 0;
                uint8_t* inBuf = AMediaCodec_getInputBuffer(impl_->audioCodec, static_cast<size_t>(inIdx), &inSize);
                if (inBuf && inSize > 0) {
                    int trackIdx = AMediaExtractor_getSampleTrackIndex(impl_->extractor);
                    if (trackIdx == impl_->audioTrackIdx) {
                        ssize_t sampleSize = AMediaExtractor_readSampleData(impl_->extractor, inBuf, inSize);
                        if (sampleSize >= 0) {
                            int64_t pts = AMediaExtractor_getSampleTime(impl_->extractor);
                            AMediaCodec_queueInputBuffer(impl_->audioCodec, static_cast<size_t>(inIdx), 0,
                                                         static_cast<size_t>(sampleSize), pts, 0);
                            AMediaExtractor_advance(impl_->extractor);
                        } else {
                            AMediaCodec_queueInputBuffer(impl_->audioCodec, static_cast<size_t>(inIdx), 0, 0, 0,
                                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                            impl_->audioEos = true;
                        }
                    } else if (trackIdx < 0) {
                        AMediaCodec_queueInputBuffer(impl_->audioCodec, static_cast<size_t>(inIdx), 0, 0, 0,
                                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        impl_->audioEos = true;
                    } else {
                        AMediaCodec_queueInputBuffer(impl_->audioCodec, static_cast<size_t>(inIdx), 0, 0, 0, 0);
                    }
                }
            }
        }

        AMediaCodecBufferInfo info{};
        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(impl_->audioCodec, &info, kTimeoutUs);
        if (outIdx >= 0) {
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                AMediaCodec_releaseOutputBuffer(impl_->audioCodec, static_cast<size_t>(outIdx), false);
                return false;
            }

            size_t outSize = 0;
            uint8_t* outBuf = AMediaCodec_getOutputBuffer(impl_->audioCodec, static_cast<size_t>(outIdx), &outSize);
            if (outBuf && info.size > 0) {
                outFrame.pcmData.assign(outBuf + info.offset, outBuf + info.offset + info.size);
                outFrame.ptsUs = info.presentationTimeUs;
                outFrame.sampleRate = audioSampleRate_;
                outFrame.channels = audioChannels_;

                AMediaCodec_releaseOutputBuffer(impl_->audioCodec, static_cast<size_t>(outIdx), false);
                return true;
            }
            AMediaCodec_releaseOutputBuffer(impl_->audioCodec, static_cast<size_t>(outIdx), false);
        } else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(impl_->audioCodec);
            if (format) {
                int32_t val = 0;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &val) && val > 0) audioSampleRate_ = val;
                if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &val) && val > 0) audioChannels_ = val;
                AMediaFormat_delete(format);
            }
        } else if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (impl_->audioEos) return false;
        }
    }
    return false;
}

// Chuyển đổi YUV420 (I420 / NV12) sang Bottom-Up B8G8R8X8 căn lề 16 với tăng tốc SIMD SSE2 / ARM NEON
void WebmDecoder::convertYuvToBottomUpBgrx(
    const DecodedVideoFrame& srcFrame,
    uint8_t* dstBgrx,
    uint32_t alignedWidth,
    uint32_t alignedHeight
) {
    if (!dstBgrx || srcFrame.pixelData.empty() || srcFrame.width == 0 || srcFrame.height == 0) return;

    const uint32_t w = srcFrame.width;
    const uint32_t h = srcFrame.height;
    const uint32_t stride = srcFrame.stride > 0 ? srcFrame.stride : w;
    const uint32_t sliceH = srcFrame.sliceHeight > 0 ? srcFrame.sliceHeight : h;

    const size_t yPlaneBytes = static_cast<size_t>(stride) * sliceH;
    if (srcFrame.pixelData.size() < yPlaneBytes) return;

    const uint8_t* yPlane = srcFrame.pixelData.data();
    const uint8_t* uvBase = yPlane + yPlaneBytes;
    const bool isPlanarI420 = (srcFrame.colorFormat == 19 /* COLOR_FormatYUV420Planar */);
    const uint32_t halfStride = stride / 2u;
    const uint8_t* uPlaneI420 = uvBase;
    const uint8_t* vPlaneI420 = uvBase + static_cast<size_t>(halfStride) * (sliceH / 2u);

    const uint32_t dstRowPitch = alignedWidth * 4u;

    for (uint32_t py = h; py < alignedHeight; ++py) {
        uint8_t* padRow = dstBgrx + static_cast<size_t>(py) * dstRowPitch;
        std::memset(padRow, 0, dstRowPitch);
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t dstY = (h - 1u - y);
        uint8_t* dstRow = dstBgrx + static_cast<size_t>(dstY) * dstRowPitch;
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * stride;
        const uint8_t* uvRowNv12 = uvBase + static_cast<size_t>(y / 2u) * stride;
        const uint8_t* uRowI420 = uPlaneI420 + static_cast<size_t>(y / 2u) * halfStride;
        const uint8_t* vRowI420 = vPlaneI420 + static_cast<size_t>(y / 2u) * halfStride;

        uint32_t x = 0;

#if defined(__SSE2__)
        const __m128i zero = _mm_setzero_si128();
        const __m128i c16 = _mm_set1_epi16(16);
        const __m128i c128 = _mm_set1_epi16(128);
        const __m128i k298 = _mm_set1_epi32(298);
        const __m128i k409 = _mm_set1_epi32(409);
        const __m128i km100 = _mm_set1_epi32(-100);
        const __m128i km208 = _mm_set1_epi32(-208);
        const __m128i k516 = _mm_set1_epi32(516);
        const __m128i kRound = _mm_set1_epi32(128);
        const __m128i alphaMask = _mm_set1_epi32(static_cast<int>(0xFF000000u));

        for (; x + 8u <= w; x += 8u) {
            __m128i y8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(yRow + x));
            __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(y8, zero), c16);

            uint8_t uTmp[8], vTmp[8];
            if (isPlanarI420) {
                const uint32_t hx = x / 2u;
                for (int i = 0; i < 4; ++i) {
                    uTmp[i * 2] = uTmp[i * 2 + 1] = uRowI420[hx + i];
                    vTmp[i * 2] = vTmp[i * 2 + 1] = vRowI420[hx + i];
                }
            } else {
                const uint8_t* uvPtr = uvRowNv12 + x;
                for (int i = 0; i < 4; ++i) {
                    uTmp[i * 2] = uTmp[i * 2 + 1] = uvPtr[i * 2];
                    vTmp[i * 2] = vTmp[i * 2 + 1] = uvPtr[i * 2 + 1];
                }
            }

            __m128i u16 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(uTmp)), zero), c128);
            __m128i v16 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(vTmp)), zero), c128);

            // Mở rộng có dấu sang 32-bit cho 4 pixel thấp và 4 pixel cao
            __m128i cLo = _mm_srai_epi32(_mm_unpacklo_epi16(zero, y16), 16);
            __m128i cHi = _mm_srai_epi32(_mm_unpackhi_epi16(zero, y16), 16);
            __m128i dLo = _mm_srai_epi32(_mm_unpacklo_epi16(zero, u16), 16);
            __m128i dHi = _mm_srai_epi32(_mm_unpackhi_epi16(zero, u16), 16);
            __m128i eLo = _mm_srai_epi32(_mm_unpacklo_epi16(zero, v16), 16);
            __m128i eHi = _mm_srai_epi32(_mm_unpackhi_epi16(zero, v16), 16);

            auto mul32 = [](const __m128i& a, const __m128i& b) -> __m128i {
                __m128i a13 = _mm_shuffle_epi32(a, 0xF5);
                __m128i b13 = _mm_shuffle_epi32(b, 0xF5);
                __m128i p02 = _mm_mul_epu32(a, b);
                __m128i p13 = _mm_mul_epu32(a13, b13);
                return _mm_unpacklo_epi64(
                    _mm_unpacklo_epi32(p02, p13),
                    _mm_unpackhi_epi32(p02, p13)
                );
            };

            __m128i yBaseLo = _mm_add_epi32(mul32(cLo, k298), kRound);
            __m128i yBaseHi = _mm_add_epi32(mul32(cHi, k298), kRound);

            __m128i rLo = _mm_srai_epi32(_mm_add_epi32(yBaseLo, mul32(eLo, k409)), 8);
            __m128i rHi = _mm_srai_epi32(_mm_add_epi32(yBaseHi, mul32(eHi, k409)), 8);
            __m128i gLo = _mm_srai_epi32(_mm_add_epi32(yBaseLo, _mm_add_epi32(mul32(dLo, km100), mul32(eLo, km208))), 8);
            __m128i gHi = _mm_srai_epi32(_mm_add_epi32(yBaseHi, _mm_add_epi32(mul32(dHi, km100), mul32(eHi, km208))), 8);
            __m128i bLo = _mm_srai_epi32(_mm_add_epi32(yBaseLo, mul32(dLo, k516)), 8);
            __m128i bHi = _mm_srai_epi32(_mm_add_epi32(yBaseHi, mul32(dHi, k516)), 8);

            __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(rLo, rHi), zero);
            __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(gLo, gHi), zero);
            __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(bLo, bHi), zero);

            __m128i bg16 = _mm_unpacklo_epi8(b8, g8);
            __m128i rx16 = _mm_unpacklo_epi8(r8, zero);
            __m128i bgrxLo = _mm_or_si128(_mm_unpacklo_epi16(bg16, rx16), alphaMask);
            __m128i bgrxHi = _mm_or_si128(_mm_unpackhi_epi16(bg16, rx16), alphaMask);

            _mm_storeu_si128(reinterpret_cast<__m128i*>(dstRow + static_cast<size_t>(x) * 4u), bgrxLo);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dstRow + static_cast<size_t>(x + 4u) * 4u), bgrxHi);
        }
#elif defined(__ARM_NEON)
        const int16x8_t c16 = vdupq_n_s16(16);
        const int16x8_t c128 = vdupq_n_s16(128);
        const int32x4_t kRound = vdupq_n_s32(128);
        const uint8x8_t alpha8 = vdup_n_u8(0xFF);

        for (; x + 8u <= w; x += 8u) {
            uint8x8_t y8 = vld1_u8(yRow + x);
            int16x8_t c16v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), c16);

            uint8_t uTmp[8], vTmp[8];
            if (isPlanarI420) {
                const uint32_t hx = x / 2u;
                for (int i = 0; i < 4; ++i) {
                    uTmp[i * 2] = uTmp[i * 2 + 1] = uRowI420[hx + i];
                    vTmp[i * 2] = vTmp[i * 2 + 1] = vRowI420[hx + i];
                }
            } else {
                const uint8_t* uvPtr = uvRowNv12 + x;
                for (int i = 0; i < 4; ++i) {
                    uTmp[i * 2] = uTmp[i * 2 + 1] = uvPtr[i * 2];
                    vTmp[i * 2] = vTmp[i * 2 + 1] = uvPtr[i * 2 + 1];
                }
            }

            int16x8_t d16v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(uTmp))), c128);
            int16x8_t e16v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(vTmp))), c128);

            int32x4_t yLo = vmlal_n_s16(kRound, vget_low_s16(c16v), 298);
            int32x4_t yHi = vmlal_n_s16(kRound, vget_high_s16(c16v), 298);

            int32x4_t rLo = vmlal_n_s16(yLo, vget_low_s16(e16v), 409);
            int32x4_t rHi = vmlal_n_s16(yHi, vget_high_s16(e16v), 409);

            int32x4_t gLo = vmlsl_n_s16(vmlsl_n_s16(yLo, vget_low_s16(d16v), 100), vget_low_s16(e16v), 208);
            int32x4_t gHi = vmlsl_n_s16(vmlsl_n_s16(yHi, vget_high_s16(d16v), 100), vget_high_s16(e16v), 208);

            int32x4_t bLo = vmlal_n_s16(yLo, vget_low_s16(d16v), 516);
            int32x4_t bHi = vmlal_n_s16(yHi, vget_high_s16(d16v), 516);

            int16x8_t r16 = vcombine_s16(vshrn_n_s32(rLo, 8), vshrn_n_s32(rHi, 8));
            int16x8_t g16 = vcombine_s16(vshrn_n_s32(gLo, 8), vshrn_n_s32(gHi, 8));
            int16x8_t b16 = vcombine_s16(vshrn_n_s32(bLo, 8), vshrn_n_s32(bHi, 8));

            uint8x8x4_t bgrx;
            bgrx.val[0] = vqmovun_s16(b16);
            bgrx.val[1] = vqmovun_s16(g16);
            bgrx.val[2] = vqmovun_s16(r16);
            bgrx.val[3] = alpha8;
            vst4_u8(dstRow + static_cast<size_t>(x) * 4u, bgrx);
        }
#endif

        // Nhánh vô hướng xử lý phần pixel lẻ ở cuối dòng (hoặc toàn bộ dòng nếu không có SIMD)
        for (; x < w; ++x) {
            const int yVal = yRow[x];
            int uVal = 128;
            int vVal = 128;

            if (isPlanarI420) {
                const uint32_t hx = x / 2u;
                uVal = uRowI420[hx];
                vVal = vRowI420[hx];
            } else {
                const size_t uvIdx = static_cast<size_t>(x / 2u) * 2u;
                uVal = uvRowNv12[uvIdx];
                vVal = uvRowNv12[uvIdx + 1u];
            }

            const int c = yVal - 16;
            const int d = uVal - 128;
            const int e = vVal - 128;

            const int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
            const int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
            const int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);

            const size_t px = static_cast<size_t>(x) * 4u;
            dstRow[px + 0u] = static_cast<uint8_t>(b);
            dstRow[px + 1u] = static_cast<uint8_t>(g);
            dstRow[px + 2u] = static_cast<uint8_t>(r);
            dstRow[px + 3u] = 0xFF;
        }

        for (uint32_t padX = w; padX < alignedWidth; ++padX) {
            const size_t px = static_cast<size_t>(padX) * 4u;
            dstRow[px + 0u] = 0;
            dstRow[px + 1u] = 0;
            dstRow[px + 2u] = 0;
            dstRow[px + 3u] = 0xFF;
        }
    }
}

} // namespace sgre
