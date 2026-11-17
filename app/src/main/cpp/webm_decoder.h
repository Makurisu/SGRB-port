#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace sgre {

struct DecodedVideoFrame {
    std::vector<uint8_t> pixelData;
    int64_t ptsUs = 0;
    int64_t durationUs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t sliceHeight = 0;
    uint32_t colorFormat = 0; // COLOR_FormatYUV420Flexible, NV12, etc.
};

struct DecodedAudioFrame {
    std::vector<uint8_t> pcmData;
    int64_t ptsUs = 0;
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
};

class WebmDecoder {
public:
    WebmDecoder();
    ~WebmDecoder();

    bool initFromByteStream(void* pByteStream, const std::string& nameHint);
    void close();

    bool hasVideo() const { return hasVideo_; }
    bool hasAudio() const { return hasAudio_; }
    uint32_t getVideoWidth() const { return videoWidth_; }
    uint32_t getVideoHeight() const { return videoHeight_; }
    int64_t getDuration100ns() const { return duration100ns_; }
    void getAudioFormat(uint32_t& sampleRate, uint32_t& channels) const {
        sampleRate = audioSampleRate_;
        channels = audioChannels_;
    }

    bool readNextVideoFrame(DecodedVideoFrame& outFrame);
    bool readNextAudioFrame(DecodedAudioFrame& outFrame);
    void seekTo(int64_t ptsUs);

    static void convertYuvToBottomUpBgrx(
        const DecodedVideoFrame& srcFrame,
        uint8_t* dstBgrx,
        uint32_t alignedWidth,
        uint32_t alignedHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool hasVideo_ = false;
    bool hasAudio_ = false;
    uint32_t videoWidth_ = 0;
    uint32_t videoHeight_ = 0;
    int64_t duration100ns_ = 0;
    uint32_t audioSampleRate_ = 48000;
    uint32_t audioChannels_ = 2;
};

} // namespace sgre
