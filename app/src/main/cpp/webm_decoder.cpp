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

// Chuyển đổi YUV420 sang Bottom-Up B8G8R8X8 căn lề 16
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

    const uint8_t* yPlane = srcFrame.pixelData.data();
    const uint8_t* uvPlane = yPlane + static_cast<size_t>(stride) * sliceH;

    const uint32_t dstRowPitch = alignedWidth * 4;

    // Xóa đen toàn bộ các dòng padding từ h đến alignedHeight để loại bỏ hoàn toàn vạch màu rác
    for (uint32_t py = h; py < alignedHeight; ++py) {
        uint8_t* padRow = dstBgrx + static_cast<size_t>(py) * dstRowPitch;
        memset(padRow, 0, dstRowPitch);
    }

    for (uint32_t y = 0; y < h; ++y) {
        // Hướng dòng ảnh Bottom-Up chuẩn Windows DIB: src dòng y (0..h-1) ánh xạ chính xác vào (h - 1 - y)
        // để 1080 dòng ảnh khớp hoàn hảo từ dòng 0 đến dòng 1079 của texture D3D11, triệt tiêu 100% vạch xanh lá ở đáy!
        uint32_t dstY = (h - 1 - y);
        uint8_t* dstRow = dstBgrx + static_cast<size_t>(dstY) * dstRowPitch;
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * stride;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * stride;

        for (uint32_t x = 0; x < w; ++x) {
            int yVal = yRow[x];
            int uVal = 128;
            int vVal = 128;

            if (srcFrame.colorFormat == 19 /* COLOR_FormatYUV420Planar / I420 */) {
                const uint8_t* uPlane = uvPlane;
                const uint8_t* vPlane = uPlane + (stride / 2) * (sliceH / 2);
                uVal = uPlane[(y / 2) * (stride / 2) + (x / 2)];
                vVal = vPlane[(y / 2) * (stride / 2) + (x / 2)];
            } else {
                // NV12 / COLOR_FormatYUV420SemiPlanar / Flexible: U, V xen kẽ
                size_t uvIdx = static_cast<size_t>(x / 2) * 2;
                uVal = uvRow[uvIdx];
                vVal = uvRow[uvIdx + 1];
            }

            int c = yVal - 16;
            int d = uVal - 128;
            int e = vVal - 128;

            int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
            int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
            int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);

            size_t px = static_cast<size_t>(x) * 4;
            dstRow[px + 0] = static_cast<uint8_t>(b);
            dstRow[px + 1] = static_cast<uint8_t>(g);
            dstRow[px + 2] = static_cast<uint8_t>(r);
            dstRow[px + 3] = 0xFF; // Alpha cứng
        }

        // Padding cho phần pixel căn lề từ w đến alignedWidth
        for (uint32_t x = w; x < alignedWidth; ++x) {
            size_t px = static_cast<size_t>(x) * 4;
            dstRow[px + 0] = 0;
            dstRow[px + 1] = 0;
            dstRow[px + 2] = 0;
            dstRow[px + 3] = 0xFF;
        }
    }
}

} // namespace sgre
