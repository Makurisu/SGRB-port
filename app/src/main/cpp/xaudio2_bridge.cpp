#include "xaudio2_bridge.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "SGRE_XAudio2Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// Stub generic an toàn trả về S_OK (0)
static int32_t WINAPI stub_XAudioSuccess() { return 0; }

// --- Mock IXAudio2MasteringVoice ---
static void* gMasteringVoiceVtable[32];
static void* gMockMasteringVoice = nullptr;

static void WINAPI mastering_DestroyVoice(void*) {
    LOGI("IXAudio2MasteringVoice::DestroyVoice called");
}

static void initMasteringVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) {
        gMasteringVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    }
    // Slot 18 trong IXAudio2Voice là DestroyVoice (trả về void)
    gMasteringVoiceVtable[18] = reinterpret_cast<void*>(&mastering_DestroyVoice);
    gMockMasteringVoice = &gMasteringVoiceVtable[0];
    initialized = true;
}

// --- Mock IXAudio2 ---
static void* gXAudio2Vtable[32];
static void* gMockXAudio2 = nullptr;

// Slot 0: QueryInterface
static int32_t WINAPI xaudio_QueryInterface(void*, const void*, void** ppvObject) {
    if (ppvObject) *ppvObject = &gMockXAudio2;
    return 0; // S_OK
}

// Slot 1: AddRef
static uint32_t WINAPI xaudio_AddRef(void*) { return 1; }

// Slot 2: Release
static uint32_t WINAPI xaudio_Release(void*) { return 1; }

// Slot 7: CreateMasteringVoice(this, ppMasteringVoice, InputChannels, InputSampleRate, Flags, DeviceId, pEffectChain, StreamCategory)
static int32_t WINAPI xaudio_CreateMasteringVoice(
    void*,
    void** ppMasteringVoice,
    uint32_t,
    uint32_t,
    uint32_t,
    const void*,
    const void*,
    uint32_t) {
    initMasteringVoiceVtable();
    if (ppMasteringVoice) {
        *ppMasteringVoice = &gMockMasteringVoice;
        LOGI("IXAudio2::CreateMasteringVoice -> Đã cấp phát Mock Mastering Voice: %p", &gMockMasteringVoice);
    }
    return 0; // S_OK
}

static void initXAudio2Vtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) {
        gXAudio2Vtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    }
    gXAudio2Vtable[0] = reinterpret_cast<void*>(&xaudio_QueryInterface);
    gXAudio2Vtable[1] = reinterpret_cast<void*>(&xaudio_AddRef);
    gXAudio2Vtable[2] = reinterpret_cast<void*>(&xaudio_Release);
    gXAudio2Vtable[7] = reinterpret_cast<void*>(&xaudio_CreateMasteringVoice);

    gMockXAudio2 = &gXAudio2Vtable[0];
    initialized = true;
}

} // namespace

extern "C" {

int32_t WINAPI XAudio2Create(void** ppXAudio2, uint32_t Flags, uint32_t XAudio2Processor) {
    LOGI("XAudio2Create intercepted: Flags=0x%X, Processor=0x%X, ppXAudio2=%p",
         Flags, XAudio2Processor, ppXAudio2);
    if (!ppXAudio2) return 0x80070057; // E_INVALIDARG

    initXAudio2Vtable();
    *ppXAudio2 = &gMockXAudio2;
    LOGI("-> Đã cấp phát Mock IXAudio2: %p", &gMockXAudio2);
    return 0; // S_OK
}

}
