#include "wma_decoder.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "SGRE_WmaDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#include "codeccontext.h"

// rockbox libwma API
extern "C" {
int wma_decode_init_fixed(CodecContext * avctx);
int wma_decode_superframe(CodecContext *avctx, void *data, int *data_size, uint8_t *buf, int buf_size);
int wma_decode_end(CodecContext *avctx);
void wma_decode_reset(CodecContext *avctx);
int wma_get_priv_data_size(void);
}

namespace sgre {

struct WmaDecoder::Impl {
    CodecContext avctx{};
    AVCodec codec{};
    std::vector<uint8_t> extradata;
    bool codecOpen = false;

    Impl() {
        memset(&avctx, 0, sizeof(avctx));
        memset(&codec, 0, sizeof(codec));
    }

    ~Impl() {
        cleanup();
    }

    void cleanup() {
        if (codecOpen) {
            wma_decode_end(&avctx);
            if (avctx.priv_data) {
                free(avctx.priv_data);
                avctx.priv_data = nullptr;
            }
            codecOpen = false;
        }
    }
};

WmaDecoder::WmaDecoder()
    : impl_(std::make_unique<Impl>()) {
}

WmaDecoder::~WmaDecoder() {
    if (impl_) {
        impl_->cleanup();
    }
}

bool WmaDecoder::init(uint32_t sampleRate, uint32_t channels, uint32_t avgBytesPerSec,
                      uint32_t blockAlign, const uint8_t* extraData, size_t extraDataSize) {
    if (!impl_) return false;
    impl_->cleanup();

    sampleRate_ = sampleRate;
    channels_ = channels;
    avgBytesPerSec_ = avgBytesPerSec;
    blockAlign_ = blockAlign;

    if (extraData && extraDataSize > 0) {
        impl_->extradata.assign(extraData, extraData + extraDataSize);
    } else {
        // Fallback mặc định 10 bytes extradata cho WMA v2 (flags1=0, flags2=0x001F)
        impl_->extradata = { 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 };
    }

    memset(&impl_->avctx, 0, sizeof(impl_->avctx));
    memset(&impl_->codec, 0, sizeof(impl_->codec));

    impl_->codec.id = CODEC_ID_WMAV2;
    impl_->avctx.codec = &impl_->codec;
    impl_->avctx.sample_rate = static_cast<int>(sampleRate_);
    impl_->avctx.channels = static_cast<int>(channels_);
    impl_->avctx.bit_rate = static_cast<int>(avgBytesPerSec_ * 8);
    impl_->avctx.block_align = static_cast<int>(blockAlign_);
    impl_->avctx.extradata = impl_->extradata.data();
    impl_->avctx.extradata_size = static_cast<int>(impl_->extradata.size());

    impl_->avctx.priv_data = calloc(1, wma_get_priv_data_size());
    if (!impl_->avctx.priv_data) {
        LOGE("WmaDecoder::init failed: cannot allocate WMADecodeContext");
        return false;
    }

    int ret = wma_decode_init_fixed(&impl_->avctx);
    if (ret < 0) {
        LOGE("WmaDecoder::init failed: wma_decode_init_fixed returned %d", ret);
        free(impl_->avctx.priv_data);
        impl_->avctx.priv_data = nullptr;
        return false;
    }

    impl_->codecOpen = true;
    isInitialized_ = true;
    LOGI("WmaDecoder initialized: rate=%u, ch=%u, bps=%u, align=%u, extraSize=%zu",
         sampleRate_, channels_, avgBytesPerSec_ * 8, blockAlign_, impl_->extradata.size());
    return true;
}

bool WmaDecoder::decode(const uint8_t* inData, size_t inBytes, std::vector<int16_t>& outSamples) {
    if (!isInitialized_ || !impl_ || !impl_->codecOpen || !inData || inBytes == 0) {
        return false;
    }

    const size_t packetAlign = blockAlign_ > 0 ? static_cast<size_t>(blockAlign_) : inBytes;
    size_t offset = 0;
    std::vector<int16_t> frameBuffer(65536);

    // Cấp bộ đệm zero-padded cho toàn bộ bitstream để bitstream reader của libwma đọc an toàn
    std::vector<uint8_t> paddedStream(inBytes + 256, 0);
    std::memcpy(paddedStream.data(), inData, inBytes);

    while (offset < inBytes) {
        size_t packetSize = std::min(packetAlign, inBytes - offset);
        if (packetSize == 0) break;
        int dataSize = 0;

        int consumed = wma_decode_superframe(
            &impl_->avctx,
            frameBuffer.data(),
            &dataSize,
            paddedStream.data() + offset,
            static_cast<int>(packetSize)
        );

        static int sPacketLog = 0;
        if (++sPacketLog <= 10) {
            LOGI("WmaDecoder: packet #%d: offset=%zu/%zu, packetSize=%zu, consumed=%d, dataSize=%d",
                 sPacketLog, offset, inBytes, packetSize, consumed, dataSize);
        }

        if (dataSize > 0) {
            size_t numSamples = static_cast<size_t>(dataSize) / sizeof(int16_t);
            if (numSamples <= frameBuffer.size()) {
                outSamples.insert(outSamples.end(), frameBuffer.begin(), frameBuffer.begin() + numSamples);
            }
        }

        // Luôn luôn tiến đúng packetSize để căn chỉnh biên blockAlign chuẩn 100% của WMA v2
        offset += packetSize;
    }

    return !outSamples.empty();
}

void WmaDecoder::reset() {
    if (isInitialized_ && impl_ && impl_->codecOpen && impl_->avctx.priv_data) {
        wma_decode_reset(&impl_->avctx);
    }
}

} // namespace sgre
