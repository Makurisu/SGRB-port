#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace sgre {

class WmaDecoder {
public:
    WmaDecoder();
    ~WmaDecoder();

    // Khởi tạo bộ giải mã WMA v2 với các thông số từ WAVEFORMATEX và extra data
    bool init(uint32_t sampleRate, uint32_t channels, uint32_t avgBytesPerSec,
              uint32_t blockAlign, const uint8_t* extraData, size_t extraDataSize);

    // Giải mã một buffer gồm một hoặc nhiều WMA superframe / packet
    // Kết quả Linear PCM 16-bit được ghi nối tiếp vào outSamples
    bool decode(const uint8_t* inData, size_t inBytes, std::vector<int16_t>& outSamples);

    void reset();

    bool isInitialized() const { return isInitialized_; }
    uint32_t getSampleRate() const { return sampleRate_; }
    uint32_t getChannels() const { return channels_; }
    uint32_t getBlockAlign() const { return blockAlign_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool isInitialized_ = false;
    uint32_t sampleRate_ = 44100;
    uint32_t channels_ = 1;
    uint32_t avgBytesPerSec_ = 4000;
    uint32_t blockAlign_ = 1487;
};

} // namespace sgre
