#include "xaudio2_bridge.h"
#include <android/log.h>
#include <cstring>
#include <cstdint>

#define LOG_TAG "SGRE_XAudio2Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

static int32_t WINAPI stub_XAudioSuccess() { return 0; }

// --- Mock IXAudio2Voice methods (chung cho Source, Submix, Mastering) ---
// Slot 18: DestroyVoice
static void WINAPI voice_DestroyVoice(void*) {
    LOGI("IXAudio2Voice::DestroyVoice called");
}

// --- Mock IXAudio2SourceVoice ---
static void* gSourceVoiceVtable[32];
static void* gMockSourceVoice = nullptr;

// Slot 19: Start(this, Flags, OperationSet)
static int32_t WINAPI sourceVoice_Start(void*, uint32_t, uint32_t) {
    LOGI("IXAudio2SourceVoice::Start called");
    return 0; // S_OK
}

// Slot 20: Stop(this, Flags, OperationSet)
static int32_t WINAPI sourceVoice_Stop(void*, uint32_t, uint32_t) {
    LOGI("IXAudio2SourceVoice::Stop called");
    return 0; // S_OK
}

// Slot 21: SubmitSourceBuffer(this, pBuffer, pBufferWMA) (Offset 0xA8)
static int32_t WINAPI sourceVoice_SubmitSourceBuffer(void*, const void* pBuffer, const void*) {
    LOGI("IXAudio2SourceVoice::SubmitSourceBuffer called (pBuffer=%p)", pBuffer);
    return 0; // S_OK
}

// Slot 22: FlushSourceBuffers(this)
static int32_t WINAPI sourceVoice_FlushSourceBuffers(void*) {
    return 0; // S_OK
}

// Slot 25: GetState(this, pVoiceState, Flags)
static void WINAPI sourceVoice_GetState(void*, void* pVoiceState, uint32_t) {
    if (pVoiceState) {
        memset(pVoiceState, 0, 24); // sizeof(XAUDIO2_VOICE_STATE) = 24 bytes
    }
}

static void initSourceVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) {
        gSourceVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    }
    gSourceVoiceVtable[18] = reinterpret_cast<void*>(&voice_DestroyVoice);
    gSourceVoiceVtable[19] = reinterpret_cast<void*>(&sourceVoice_Start);
    gSourceVoiceVtable[20] = reinterpret_cast<void*>(&sourceVoice_Stop);
    gSourceVoiceVtable[21] = reinterpret_cast<void*>(&sourceVoice_SubmitSourceBuffer);
    gSourceVoiceVtable[22] = reinterpret_cast<void*>(&sourceVoice_FlushSourceBuffers);
    gSourceVoiceVtable[25] = reinterpret_cast<void*>(&sourceVoice_GetState);

    gMockSourceVoice = &gSourceVoiceVtable[0];
    initialized = true;
}

// --- Mock IXAudio2MasteringVoice ---
static void* gMasteringVoiceVtable[32];
static void* gMockMasteringVoice = nullptr;

static void initMasteringVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) {
        gMasteringVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    }
    gMasteringVoiceVtable[18] = reinterpret_cast<void*>(&voice_DestroyVoice);
    gMockMasteringVoice = &gMasteringVoiceVtable[0];
    initialized = true;
}

// --- Mock IXAudio2SubmixVoice ---
static void* gSubmixVoiceVtable[32];
static void* gMockSubmixVoice = nullptr;

static void initSubmixVoiceVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 32; ++i) {
        gSubmixVoiceVtable[i] = reinterpret_cast<void*>(&stub_XAudioSuccess);
    }
    gSubmixVoiceVtable[18] = reinterpret_cast<void*>(&voice_DestroyVoice);
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
    void*,
    void** ppSourceVoice,
    const void*,
    uint32_t,
    float,
    void*,
    const void*,
    const void*) {
    initSourceVoiceVtable();
    if (ppSourceVoice) {
        *ppSourceVoice = &gMockSourceVoice;
        LOGI("IXAudio2::CreateSourceVoice -> Đã cấp phát Mock Source Voice: %p", &gMockSourceVoice);
    }
    return 0; // S_OK
}

// Slot 6 (Offset 0x30): CreateSubmixVoice
static int32_t WINAPI xaudio_CreateSubmixVoice(
    void*,
    void** ppSubmixVoice,
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t,
    const void*,
    const void*) {
    initSubmixVoiceVtable();
    if (ppSubmixVoice) {
        *ppSubmixVoice = &gMockSubmixVoice;
        LOGI("IXAudio2::CreateSubmixVoice -> Đã cấp phát Mock Submix Voice: %p", &gMockSubmixVoice);
    }
    return 0; // S_OK
}

// Slot 7 (Offset 0x38): CreateMasteringVoice
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
    // IUnknown
    gXAudio2Vtable[0] = reinterpret_cast<void*>(&xaudio_QueryInterface);
    gXAudio2Vtable[1] = reinterpret_cast<void*>(&xaudio_AddRef);
    gXAudio2Vtable[2] = reinterpret_cast<void*>(&xaudio_Release);

    // IXAudio2 (chuẩn vtable x64):
    // Slot 5 (offset 0x28): CreateSourceVoice
    // Slot 6 (offset 0x30): CreateSubmixVoice
    // Slot 7 (offset 0x38): CreateMasteringVoice
    gXAudio2Vtable[5] = reinterpret_cast<void*>(&xaudio_CreateSourceVoice);
    gXAudio2Vtable[6] = reinterpret_cast<void*>(&xaudio_CreateSubmixVoice);
    gXAudio2Vtable[7] = reinterpret_cast<void*>(&xaudio_CreateMasteringVoice);

    // Dự phòng an toàn cho biến thể IXAudio2 2.7 / legacy interface:
    gXAudio2Vtable[8] = reinterpret_cast<void*>(&xaudio_CreateSubmixVoice);

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
