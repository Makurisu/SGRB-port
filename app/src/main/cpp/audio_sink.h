#pragma once
#include <cstdint>
#include <cstddef>
#include "win32_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Cấu trúc dữ liệu chuẩn Windows WinMM ---
#pragma pack(push, 1)
struct WinWaveFormatEx {
    uint16_t wFormatTag;      // 1 = WAVE_FORMAT_PCM
    uint16_t nChannels;       // 1, 2
    uint32_t nSamplesPerSec;  // 44100, 48000...
    uint32_t nAvgBytesPerSec; // nSamplesPerSec * nBlockAlign
    uint16_t nBlockAlign;     // nChannels * (wBitsPerSample / 8)
    uint16_t wBitsPerSample;  // 16
    uint16_t cbSize;          // 0
};

struct WinWaveHdr {
    char* lpData;
    uint32_t dwBufferLength;
    uint32_t dwBytesRecorded;
    uintptr_t dwUser;
    uint32_t dwFlags;
    uint32_t dwLoops;
    struct WinWaveHdr* lpNext;
    uintptr_t reserved;
};

struct WinWaveOutCapsA {
    uint16_t wMid;
    uint16_t wPid;
    uint32_t vDriverVersion;
    char szPname[32];
    uint32_t dwFormats;
    uint16_t wChannels;
    uint16_t wReserved1;
    uint32_t dwSupport;
};

struct WinMmTime {
    uint32_t wType;
    union {
        uint32_t ms;
        uint32_t sample;
        uint32_t cb;
        uint32_t ticks;
        struct {
            uint8_t hour;
            uint8_t min;
            uint8_t sec;
            uint8_t frame;
            uint8_t fps;
            uint8_t dummy;
            uint8_t pad[2];
        } smpte;
        struct {
            uint32_t songptrpos;
        } midi;
    } u;
};
#pragma pack(pop)

// Hằng số mã lỗi WinMM (MMSYSERR)
constexpr uint32_t WINMM_MMSYSERR_NOERROR     = 0;
constexpr uint32_t WINMM_MMSYSERR_ERROR       = 1;
constexpr uint32_t WINMM_MMSYSERR_BADDEVICEID = 2;
constexpr uint32_t WINMM_MMSYSERR_INVALHANDLE = 5;
constexpr uint32_t WINMM_MMSYSERR_INVALPARAM  = 11;

// Hằng số Callback WinMM
constexpr uint32_t WINMM_CALLBACK_TYPEMASK    = 0x00070000;
constexpr uint32_t WINMM_CALLBACK_NULL        = 0x00000000;
constexpr uint32_t WINMM_CALLBACK_WINDOW      = 0x00010000;
constexpr uint32_t WINMM_CALLBACK_TASK        = 0x00020000;
constexpr uint32_t WINMM_CALLBACK_FUNCTION    = 0x00030000;
constexpr uint32_t WINMM_CALLBACK_EVENT       = 0x00050000;

// Hằng số cờ WAVEHDR (dwFlags)
constexpr uint32_t WINMM_WHDR_DONE            = 0x00000001;
constexpr uint32_t WINMM_WHDR_PREPARED        = 0x00000002;
constexpr uint32_t WINMM_WHDR_BEGINLOOP       = 0x00000004;
constexpr uint32_t WINMM_WHDR_ENDLOOP         = 0x00000008;
constexpr uint32_t WINMM_WHDR_INQUEUE         = 0x00000010;

// Thông điệp callback WaveOut
constexpr uint32_t WINMM_WOM_OPEN             = 0x3BB;
constexpr uint32_t WINMM_WOM_CLOSE            = 0x3BC;
constexpr uint32_t WINMM_WOM_DONE             = 0x3BD;

// Hằng số đơn vị thời gian WinMM
constexpr uint32_t WINMM_TIME_MS              = 0x0001;
constexpr uint32_t WINMM_TIME_SAMPLES         = 0x0002;
constexpr uint32_t WINMM_TIME_BYTES           = 0x0004;

// --- Giao diện API WaveOut cho miniaudio / game loop ---
uint32_t AudioSink_WaveOutGetNumDevs();
uint32_t AudioSink_WaveOutGetDevCapsA(uintptr_t uDevId, void* pwoc, uint32_t cbwoc);
uint32_t AudioSink_WaveOutOpen(void** phwo, uint32_t uDevId, const void* pwfx, uintptr_t dwCallback, uintptr_t dwInstance, uint32_t fdwOpen);
uint32_t AudioSink_WaveOutClose(void* hwo);
uint32_t AudioSink_WaveOutPrepareHeader(void* hwo, void* pwh, uint32_t cbwh);
uint32_t AudioSink_WaveOutUnprepareHeader(void* hwo, void* pwh, uint32_t cbwh);
uint32_t AudioSink_WaveOutWrite(void* hwo, void* pwh, uint32_t cbwh);
uint32_t AudioSink_WaveOutReset(void* hwo);
uint32_t AudioSink_WaveOutPause(void* hwo);
uint32_t AudioSink_WaveOutRestart(void* hwo);
uint32_t AudioSink_WaveOutGetPosition(void* hwo, void* pmmt, uint32_t cbmmt);

// --- Giao diện API XAudio2 cho movie / cutscene audio ---
bool AudioSink_XAudio2Open(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample);
void AudioSink_XAudio2Write(const void* data, size_t bytes);
void AudioSink_XAudio2Flush();
void AudioSink_XAudio2Close();
bool AudioSink_XAudio2IsOpen();

// --- Điều khiển tạm dừng / khôi phục toàn cục theo vòng đời Activity Android ---
void AudioSink_SetLifecyclePaused(bool paused);

#ifdef __cplusplus
}
#endif
