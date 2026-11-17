#include "media_foundation_bridge.h"
#include "d3d11_bridge.h"
#include "webm_decoder.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <codecvt>
#include <locale>

#define LOG_TAG "SGRE_MediaFoundation"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// --- Định nghĩa GUIDs chuẩn xác 100% đối chiếu từ IDA Pro ---
struct MF_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool matches(const void* ptr) const {
        return ptr && memcmp(this, ptr, sizeof(MF_GUID)) == 0;
    }
};

// IUnknown: {00000000-0000-0000-C000-000000000046}
static const MF_GUID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
// IMFAttributes: {2cd2d928-1476-4dc7-bd5b-7131264c50c5}
static const MF_GUID IID_IMFAttributes = { 0x2CD2D928, 0x1476, 0x4DC7, { 0xBD, 0x5B, 0x71, 0x31, 0x26, 0x4C, 0x50, 0xC5 } };
// IMFMediaType: {44cb000e-75e2-4973-8096-de1a9ee0bed7}
static const MF_GUID IID_IMFMediaType = { 0x44CB000E, 0x75E2, 0x4973, { 0x80, 0x96, 0xDE, 0x1A, 0x9E, 0xE0, 0xBE, 0xD7 } };
// IMFAudioMediaType: {26a0adc3-ce26-4672-9304-69552edd3faf} (IDA 0x140607928)
static const MF_GUID IID_IMFAudioMediaType = { 0x26A0ADC3, 0xCE26, 0x4672, { 0x93, 0x04, 0x69, 0x55, 0x2E, 0xDD, 0x3F, 0xAF } };
// IMFMediaBuffer: {045fa593-8799-42b8-bc8d-8968c6453507}
static const MF_GUID IID_IMFMediaBuffer = { 0x045FA593, 0x8799, 0x42B8, { 0xBC, 0x8D, 0x89, 0x68, 0xC6, 0x45, 0x35, 0x07 } };
// IMFDXGIBuffer: {e7174cfa-1c9e-48b1-8866-626226bfc258} (IDA 0x140607938)
static const MF_GUID IID_IMFDXGIBuffer = { 0xE7174CFA, 0x1C9E, 0x48B1, { 0x88, 0x66, 0x62, 0x62, 0x26, 0xBF, 0xC2, 0x58 } };
// ID3D11Texture2D: {6f15aaf2-d208-4e89-9ab4-489535d34f9c} (IDA 0x1406078D8)
static const MF_GUID IID_ID3D11Texture2D = { 0x6F15AAF2, 0xD208, 0x4E89, { 0x9A, 0xB4, 0x48, 0x95, 0x35, 0xD3, 0x4F, 0x9C } };
// IMFSample: {c40a00f2-b93a-4d80-ae8c-5a1c634f58e4}
static const MF_GUID IID_IMFSample = { 0xC40A00F2, 0xB93A, 0x4D80, { 0xAE, 0x8C, 0x5A, 0x1C, 0x63, 0x4F, 0x58, 0xE4 } };
// IMFTransform: {bf94c121-5b05-4e6f-8000-ba598961414d} (IDA 0x1406077D8)
static const MF_GUID IID_IMFTransform = { 0xBF94C121, 0x5B05, 0x4E6F, { 0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D } };
// CLSID_CColorConvertDMO: {98230571-0087-4204-b020-3282538e57d3} (IDA 0x1408E35D8)
static const MF_GUID CLSID_CColorConvertDMO = { 0x98230571, 0x0087, 0x4204, { 0xB0, 0x20, 0x32, 0x82, 0x53, 0x8E, 0x57, 0xD3 } };

// Các Keys thuộc tính Media Foundation (IDA 0x1408E3528 - 0x1408E35B8)
static const MF_GUID MF_MT_MAJOR_TYPE = { 0x48EBA18E, 0xF8C9, 0x4687, { 0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F } };
static const MF_GUID MF_MT_SUBTYPE = { 0xF7E34C9A, 0x42E8, 0x4714, { 0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5 } };
static const MF_GUID MF_MT_FRAME_SIZE = { 0x1652C33D, 0xD6B2, 0x4012, { 0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D } };
static const MF_GUID MF_MT_DEFAULT_STRIDE = { 0x644B4E48, 0x1E02, 0x4516, { 0xB0, 0xEB, 0xC0, 0x1C, 0xA9, 0xD4, 0x9A, 0xC6 } };
static const MF_GUID MF_MT_MINIMUM_DISPLAY_APERTURE = { 0xD7388766, 0x18FE, 0x48C6, { 0xA1, 0x77, 0xEE, 0x89, 0x48, 0x67, 0xC8, 0xC4 } };

static const MF_GUID MFMediaType_Video = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const MF_GUID MFMediaType_Audio = { 0x61756473, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const MF_GUID MFVideoFormat_RGB32 = { 0x00000016, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const MF_GUID MFVideoFormat_NV12  = { 0x3231564E, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const MF_GUID MFAudioFormat_PCM   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const MF_GUID MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS = { 0x6D23F5C8, 0xC5D7, 0x4A9B, { 0x99, 0x71, 0x5D, 0x11, 0xF8, 0xBC, 0xA8, 0x80 } };
static const MF_GUID MF_PD_DURATION = { 0x6C990D33, 0xBB8E, 0x477A, { 0x85, 0x98, 0x0D, 0x5D, 0x96, 0xFC, 0xD8, 0x8A } };

#pragma pack(push, 1)
struct MockWaveFormatEx {
    uint16_t wFormatTag = 1;       // WAVE_FORMAT_PCM
    uint16_t nChannels = 2;
    uint32_t nSamplesPerSec = 48000;
    uint32_t nAvgBytesPerSec = 192000; // 48000 * 2 * 2
    uint16_t nBlockAlign = 4;
    uint16_t wBitsPerSample = 16;
    uint16_t cbSize = 0;
};
#pragma pack(pop)

static int64_t WINAPI mock_GenericSuccess(void*) { return 0; }

// --- 1. IMFAttributes COM Implementation ---
class MFAttributesImpl {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

    MFAttributesImpl() {
        initVtable();
        lpVtbl = vtable_;
    }
    ~MFAttributesImpl() = default;

    void* getVtable() { return vtable_; }

    void addRef() { ++refCount_; }
    uint32_t release() {
        uint32_t r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    void setUINT32(const MF_GUID& key, uint32_t val) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32Map_[key.data1] = val;
    }

    void setUINT64(const MF_GUID& key, uint64_t val) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64Map_[key.data1] = val;
    }

    bool getUINT64(const MF_GUID& key, uint64_t& outVal) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = uint64Map_.find(key.data1);
        if (it != uint64Map_.end()) {
            outVal = it->second;
            return true;
        }
        return false;
    }

    void setGUID(const MF_GUID& key, const MF_GUID& val) {
        std::lock_guard<std::mutex> lock(mutex_);
        guidMap_[key.data1] = val;
    }

    bool getGUID(const MF_GUID& key, MF_GUID& outVal) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = guidMap_.find(key.data1);
        if (it != guidMap_.end()) {
            outVal = it->second;
            return true;
        }
        return false;
    }

    void copyAllItemsTo(MFAttributesImpl* dst) {
        if (!dst) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::lock_guard<std::mutex> dstLock(dst->mutex_);
        dst->uint32Map_ = uint32Map_;
        dst->uint64Map_ = uint64Map_;
        dst->guidMap_ = guidMap_;
    }

protected:
    void* vtable_[36] = { nullptr };
    std::atomic<uint32_t> refCount_{ 1 };
    std::mutex mutex_;
    std::unordered_map<uint32_t, uint32_t> uint32Map_;
    std::unordered_map<uint32_t, uint64_t> uint64Map_;
    std::unordered_map<uint32_t, MF_GUID> guidMap_;

    void initVtable();

    static int32_t WINAPI QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003); // E_POINTER
        if (IID_IUnknown.matches(riid) || IID_IMFAttributes.matches(riid)) {
            *ppv = thisPtr;
            reinterpret_cast<MFAttributesImpl*>(thisPtr)->addRef();
            return 0; // S_OK
        }
        *ppv = nullptr;
        return static_cast<int32_t>(0x80004002); // E_NOINTERFACE
    }

    static uint32_t WINAPI AddRef(void* thisPtr) {
        auto* obj = reinterpret_cast<MFAttributesImpl*>(thisPtr);
        obj->addRef();
        return obj->refCount_.load();
    }

    static uint32_t WINAPI Release(void* thisPtr) {
        return reinterpret_cast<MFAttributesImpl*>(thisPtr)->release();
    }

    static int32_t WINAPI GetUINT64(void* thisPtr, const void* pguidKey, uint64_t* punValue) {
        if (!pguidKey || !punValue) return static_cast<int32_t>(0x80004003);
        const auto* key = reinterpret_cast<const MF_GUID*>(pguidKey);
        auto* obj = reinterpret_cast<MFAttributesImpl*>(thisPtr);
        if (obj->getUINT64(*key, *punValue)) return 0;
        return static_cast<int32_t>(0xC00D36E6); // MF_E_ATTRIBUTENOTFOUND
    }

    static int32_t WINAPI GetGUID(void* thisPtr, const void* pguidKey, void* pguidValue) {
        if (!pguidKey || !pguidValue) return static_cast<int32_t>(0x80004003);
        const auto* key = reinterpret_cast<const MF_GUID*>(pguidKey);
        auto* outGuid = reinterpret_cast<MF_GUID*>(pguidValue);
        auto* obj = reinterpret_cast<MFAttributesImpl*>(thisPtr);
        if (obj->getGUID(*key, *outGuid)) return 0;
        return static_cast<int32_t>(0xC00D36E6); // MF_E_ATTRIBUTENOTFOUND
    }

    static int32_t WINAPI GetBlob(void*, const void*, void*, uint32_t, uint32_t*) {
        return static_cast<int32_t>(0xC00D36E6); // MF_E_ATTRIBUTENOTFOUND (kích hoạt fallback toạ độ chuẩn trong ProcessVideo)
    }

    static int32_t WINAPI SetUINT32(void* thisPtr, const void* pguidKey, uint32_t unValue) {
        if (!pguidKey) return static_cast<int32_t>(0x80004003);
        const auto* key = reinterpret_cast<const MF_GUID*>(pguidKey);
        reinterpret_cast<MFAttributesImpl*>(thisPtr)->setUINT32(*key, unValue);
        return 0; // S_OK
    }

    static int32_t WINAPI SetUINT64(void* thisPtr, const void* pguidKey, uint64_t unValue) {
        if (!pguidKey) return static_cast<int32_t>(0x80004003);
        const auto* key = reinterpret_cast<const MF_GUID*>(pguidKey);
        reinterpret_cast<MFAttributesImpl*>(thisPtr)->setUINT64(*key, unValue);
        return 0; // S_OK
    }

    static int32_t WINAPI SetGUID(void* thisPtr, const void* pguidKey, const void* pguidValue) {
        if (!pguidKey || !pguidValue) return static_cast<int32_t>(0x80004003);
        const auto* key = reinterpret_cast<const MF_GUID*>(pguidKey);
        const auto* val = reinterpret_cast<const MF_GUID*>(pguidValue);
        reinterpret_cast<MFAttributesImpl*>(thisPtr)->setGUID(*key, *val);
        return 0; // S_OK
    }

    static int32_t WINAPI SetUnknown(void*, const void*, void*) {
        return 0; // S_OK
    }

    static int32_t WINAPI CopyAllItems(void* thisPtr, void* pDest) {
        if (!pDest) return static_cast<int32_t>(0x80004003);
        auto* src = reinterpret_cast<MFAttributesImpl*>(thisPtr);
        auto* dst = reinterpret_cast<MFAttributesImpl*>(pDest);
        src->copyAllItemsTo(dst);
        return 0; // S_OK
    }
};

void MFAttributesImpl::initVtable() {
    for (int i = 0; i < 36; ++i) vtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
    vtable_[0] = reinterpret_cast<void*>(&MFAttributesImpl::QueryInterface);
    vtable_[1] = reinterpret_cast<void*>(&MFAttributesImpl::AddRef);
    vtable_[2] = reinterpret_cast<void*>(&MFAttributesImpl::Release);
    vtable_[8] = reinterpret_cast<void*>(&MFAttributesImpl::GetUINT64);
    vtable_[10] = reinterpret_cast<void*>(&MFAttributesImpl::GetGUID);
    vtable_[15] = reinterpret_cast<void*>(&MFAttributesImpl::GetBlob);
    vtable_[21] = reinterpret_cast<void*>(&MFAttributesImpl::SetUINT32);
    vtable_[22] = reinterpret_cast<void*>(&MFAttributesImpl::SetUINT64);
    vtable_[24] = reinterpret_cast<void*>(&MFAttributesImpl::SetGUID);
    vtable_[27] = reinterpret_cast<void*>(&MFAttributesImpl::SetUnknown);
    vtable_[32] = reinterpret_cast<void*>(&MFAttributesImpl::CopyAllItems);
}

// --- 2. IMFMediaType / IMFAudioMediaType Implementation ---
class MFMediaTypeImpl : public MFAttributesImpl {
public:
    MFMediaTypeImpl() {
        initMediaTypeVtable();
        lpVtbl = mediaTypeVtable_;
    }

    void setAudioFormat(uint32_t sampleRate, uint32_t channels) {
        waveFormat_.nSamplesPerSec = sampleRate > 0 ? sampleRate : 48000;
        waveFormat_.nChannels = channels > 0 ? static_cast<uint16_t>(channels) : 2;
        waveFormat_.nBlockAlign = waveFormat_.nChannels * 2;
        waveFormat_.nAvgBytesPerSec = waveFormat_.nSamplesPerSec * waveFormat_.nBlockAlign;
    }

    const MockWaveFormatEx* getAudioFormat() const { return &waveFormat_; }

protected:
    void* mediaTypeVtable_[42] = { nullptr };
    MockWaveFormatEx waveFormat_{};

    void initMediaTypeVtable() {
        initVtable();
        for (int i = 0; i < 36; ++i) mediaTypeVtable_[i] = vtable_[i];
        for (int i = 36; i < 42; ++i) mediaTypeVtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);

        mediaTypeVtable_[0] = reinterpret_cast<void*>(&MFMediaTypeImpl::MediaType_QueryInterface);
        // Slot 38 (offset 304 = 0x130): GetAudioFormat() trong IMFAudioMediaType (IDA 0x14038EA30)
        mediaTypeVtable_[38] = reinterpret_cast<void*>(&MFMediaTypeImpl::MediaType_GetAudioFormat);
    }

    static int32_t WINAPI MediaType_QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        if (IID_IUnknown.matches(riid) || IID_IMFAttributes.matches(riid) ||
            IID_IMFMediaType.matches(riid) || IID_IMFAudioMediaType.matches(riid)) {
            *ppv = thisPtr;
            reinterpret_cast<MFMediaTypeImpl*>(thisPtr)->addRef();
            return 0; // S_OK
        }
        *ppv = nullptr;
        return static_cast<int32_t>(0x80004002);
    }

    static const MockWaveFormatEx* WINAPI MediaType_GetAudioFormat(void* thisPtr) {
        return reinterpret_cast<MFMediaTypeImpl*>(thisPtr)->getAudioFormat();
    }
};

// --- 3. IMFMediaBuffer & IMFDXGIBuffer Implementation ---
static void* gDummyDXGIVtable[16] = { nullptr };
static void* gDummyDXGIResource = gDummyDXGIVtable;

static void initDummyDXGIVtableOnce() {
    static bool sInit = false;
    if (sInit) return;
    sInit = true;
    for (int i = 0; i < 16; ++i) gDummyDXGIVtable[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
}

class MFMediaBufferImpl {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

    struct DXGISubInterface {
        void* lpVtbl;
        MFMediaBufferImpl* parent;
    } dxgiSub_;

    explicit MFMediaBufferImpl(uint32_t maxLength) : maxLength_(maxLength) {
        data_.assign(maxLength, 0);
        initVtable();
        lpVtbl = vtable_;
    }

    void* getVtable() { return vtable_; }
    void addRef() { ++refCount_; }
    uint32_t release() {
        uint32_t r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    std::vector<uint8_t>& getData() { return data_; }
    uint32_t getCurrentLength() const { return currentLength_; }
    void setCurrentLength(uint32_t len) { currentLength_ = std::min(len, maxLength_); }

private:
    void* vtable_[16] = { nullptr };
    void* dxgiVtable_[16] = { nullptr };
    std::atomic<uint32_t> refCount_{ 1 };
    std::vector<uint8_t> data_;
    uint32_t maxLength_ = 0;
    uint32_t currentLength_ = 0;

    void initVtable() {
        initDummyDXGIVtableOnce();
        for (int i = 0; i < 16; ++i) {
            vtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
            dxgiVtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
        }

        // 1. IMFMediaBuffer vtable
        vtable_[0] = reinterpret_cast<void*>(&MFMediaBufferImpl::QueryInterface);
        vtable_[1] = reinterpret_cast<void*>(&MFMediaBufferImpl::AddRef);
        vtable_[2] = reinterpret_cast<void*>(&MFMediaBufferImpl::Release);
        vtable_[3] = reinterpret_cast<void*>(&MFMediaBufferImpl::Lock);
        vtable_[4] = reinterpret_cast<void*>(&MFMediaBufferImpl::Unlock);
        vtable_[5] = reinterpret_cast<void*>(&MFMediaBufferImpl::GetCurrentLength);
        vtable_[6] = reinterpret_cast<void*>(&MFMediaBufferImpl::SetCurrentLength);
        vtable_[7] = reinterpret_cast<void*>(&MFMediaBufferImpl::GetMaxLength);

        // 2. IMFDXGIBuffer vtable
        dxgiSub_.lpVtbl = dxgiVtable_;
        dxgiSub_.parent = this;
        dxgiVtable_[0] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_QueryInterface);
        dxgiVtable_[1] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_AddRef);
        dxgiVtable_[2] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_Release);
        dxgiVtable_[3] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_GetResource);
        dxgiVtable_[4] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_GetSubresourceIndex);
        dxgiVtable_[5] = reinterpret_cast<void*>(&MFMediaBufferImpl::DXGI_GetUnknown);
    }

    static int32_t WINAPI QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        auto* obj = reinterpret_cast<MFMediaBufferImpl*>(thisPtr);
        if (IID_IMFDXGIBuffer.matches(riid)) {
            *ppv = &obj->dxgiSub_;
            obj->addRef();
            return 0; // S_OK
        }
        if (IID_IUnknown.matches(riid) || IID_IMFMediaBuffer.matches(riid)) {
            *ppv = thisPtr;
            obj->addRef();
            return 0; // S_OK
        }
        *ppv = nullptr;
        return static_cast<int32_t>(0x80004002);
    }

    static uint32_t WINAPI AddRef(void* thisPtr) {
        auto* obj = reinterpret_cast<MFMediaBufferImpl*>(thisPtr);
        obj->addRef();
        return obj->refCount_.load();
    }

    static uint32_t WINAPI Release(void* thisPtr) {
        return reinterpret_cast<MFMediaBufferImpl*>(thisPtr)->release();
    }

    static int32_t WINAPI Lock(void* thisPtr, uint8_t** ppbBuffer, uint32_t* pcbMaxLength, uint32_t* pcbCurrentLength) {
        auto* obj = reinterpret_cast<MFMediaBufferImpl*>(thisPtr);
        if (ppbBuffer) *ppbBuffer = obj->data_.data();
        if (pcbMaxLength) *pcbMaxLength = obj->maxLength_;
        if (pcbCurrentLength) *pcbCurrentLength = obj->currentLength_;
        return 0; // S_OK
    }

    static int32_t WINAPI Unlock(void*) { return 0; }

    static int32_t WINAPI GetCurrentLength(void* thisPtr, uint32_t* pcbCurrentLength) {
        if (pcbCurrentLength) {
            *pcbCurrentLength = reinterpret_cast<MFMediaBufferImpl*>(thisPtr)->currentLength_;
        }
        return 0;
    }

    static int32_t WINAPI SetCurrentLength(void* thisPtr, uint32_t cbCurrentLength) {
        reinterpret_cast<MFMediaBufferImpl*>(thisPtr)->setCurrentLength(cbCurrentLength);
        return 0;
    }

    static int32_t WINAPI GetMaxLength(void* thisPtr, uint32_t* pcbMaxLength) {
        if (pcbMaxLength) {
            *pcbMaxLength = reinterpret_cast<MFMediaBufferImpl*>(thisPtr)->maxLength_;
        }
        return 0;
    }

    // --- IMFDXGIBuffer Implementation ---
    static int32_t WINAPI DXGI_QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        auto* sub = reinterpret_cast<DXGISubInterface*>(thisPtr);
        return MFMediaBufferImpl::QueryInterface(sub->parent, riid, ppv);
    }

    static uint32_t WINAPI DXGI_AddRef(void* thisPtr) {
        auto* sub = reinterpret_cast<DXGISubInterface*>(thisPtr);
        sub->parent->addRef();
        return sub->parent->refCount_.load();
    }

    static uint32_t WINAPI DXGI_Release(void* thisPtr) {
        auto* sub = reinterpret_cast<DXGISubInterface*>(thisPtr);
        return sub->parent->release();
    }

    static int32_t WINAPI DXGI_GetResource(void* thisPtr, const void* riid, void** ppvObject) {
        if (!ppvObject) return static_cast<int32_t>(0x80004003);
        *ppvObject = D3D11Bridge_GetMockTexture2D();
        return 0; // S_OK
    }

    static int32_t WINAPI DXGI_GetSubresourceIndex(void* thisPtr, uint32_t* puSubresource) {
        if (puSubresource) *puSubresource = 0;
        return 0; // S_OK
    }

    static int32_t WINAPI DXGI_GetUnknown(void* thisPtr, const void* riid, void** ppvObject) {
        if (ppvObject) *ppvObject = nullptr;
        return 0; // S_OK
    }
};

// --- 4. IMFSample Implementation ---
class MFSampleImpl : public MFAttributesImpl {
public:
    MFSampleImpl() {
        initSampleVtable();
        lpVtbl = sampleVtable_;
    }

    ~MFSampleImpl() {
        for (auto* buf : buffers_) {
            if (buf) buf->release();
        }
    }

    void addBuffer(MFMediaBufferImpl* buf) {
        if (buf) {
            buf->addRef();
            buffers_.push_back(buf);
        }
    }

    int64_t getSampleTime() const { return sampleTime_; }
    void setSampleTime(int64_t t) { sampleTime_ = t; }

    int64_t getSampleDuration() const { return duration_; }
    void setSampleDuration(int64_t d) { duration_ = d; }

    size_t getBufferCount() const { return buffers_.size(); }
    MFMediaBufferImpl* getBuffer(size_t idx) const {
        if (idx < buffers_.size()) return buffers_[idx];
        return nullptr;
    }

protected:
    void* sampleVtable_[48] = { nullptr };
    std::vector<MFMediaBufferImpl*> buffers_;
    int64_t sampleTime_ = 0;
    int64_t duration_ = 166667; // 100ns (16.6ms cho 60fps)

    void initSampleVtable() {
        initVtable();
        for (int i = 0; i < 33; ++i) sampleVtable_[i] = vtable_[i];
        for (int i = 33; i < 48; ++i) sampleVtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);

        sampleVtable_[0] = reinterpret_cast<void*>(&MFSampleImpl::Sample_QueryInterface);
        // Slot 35 (offset 280): GetSampleTime
        sampleVtable_[35] = reinterpret_cast<void*>(&MFSampleImpl::Sample_GetSampleTime);
        // Slot 36 (offset 288): SetSampleTime
        sampleVtable_[36] = reinterpret_cast<void*>(&MFSampleImpl::Sample_SetSampleTime);
        // Slot 37 (offset 296): GetSampleDuration
        sampleVtable_[37] = reinterpret_cast<void*>(&MFSampleImpl::Sample_GetSampleDuration);
        // Slot 38 (offset 304): SetSampleDuration
        sampleVtable_[38] = reinterpret_cast<void*>(&MFSampleImpl::Sample_SetSampleDuration);
        // Slot 39 (offset 312): GetBufferCount
        sampleVtable_[39] = reinterpret_cast<void*>(&MFSampleImpl::Sample_GetBufferCount);
        // Slot 40 (offset 320): GetBufferByIndex
        sampleVtable_[40] = reinterpret_cast<void*>(&MFSampleImpl::Sample_GetBufferByIndex);
        // Slot 41 (offset 328): ConvertToContiguousBuffer
        sampleVtable_[41] = reinterpret_cast<void*>(&MFSampleImpl::Sample_ConvertToContiguousBuffer);
        // Slot 42 (offset 336): AddBuffer
        sampleVtable_[42] = reinterpret_cast<void*>(&MFSampleImpl::Sample_AddBuffer);
    }

    static int32_t WINAPI Sample_QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        if (IID_IUnknown.matches(riid) || IID_IMFAttributes.matches(riid) || IID_IMFSample.matches(riid)) {
            *ppv = thisPtr;
            reinterpret_cast<MFSampleImpl*>(thisPtr)->addRef();
            return 0; // S_OK
        }
        *ppv = nullptr;
        return static_cast<int32_t>(0x80004002);
    }

    static int32_t WINAPI Sample_GetSampleTime(void* thisPtr, int64_t* phnsSampleTime) {
        if (phnsSampleTime) {
            *phnsSampleTime = reinterpret_cast<MFSampleImpl*>(thisPtr)->getSampleTime();
        }
        return 0;
    }

    static int32_t WINAPI Sample_SetSampleTime(void* thisPtr, int64_t hnsSampleTime) {
        reinterpret_cast<MFSampleImpl*>(thisPtr)->setSampleTime(hnsSampleTime);
        return 0;
    }

    static int32_t WINAPI Sample_GetSampleDuration(void* thisPtr, int64_t* phnsSampleDuration) {
        if (phnsSampleDuration) {
            *phnsSampleDuration = reinterpret_cast<MFSampleImpl*>(thisPtr)->getSampleDuration();
        }
        return 0;
    }

    static int32_t WINAPI Sample_SetSampleDuration(void* thisPtr, int64_t hnsSampleDuration) {
        reinterpret_cast<MFSampleImpl*>(thisPtr)->setSampleDuration(hnsSampleDuration);
        return 0;
    }

    static int32_t WINAPI Sample_GetBufferCount(void* thisPtr, uint32_t* pdwBufferCount) {
        if (pdwBufferCount) {
            *pdwBufferCount = static_cast<uint32_t>(reinterpret_cast<MFSampleImpl*>(thisPtr)->getBufferCount());
        }
        return 0;
    }

    static int32_t WINAPI Sample_GetBufferByIndex(void* thisPtr, uint32_t dwIndex, void** ppBuffer) {
        if (!ppBuffer) return static_cast<int32_t>(0x80004003);
        auto* buf = reinterpret_cast<MFSampleImpl*>(thisPtr)->getBuffer(dwIndex);
        if (buf) {
            buf->addRef();
            *ppBuffer = buf;
            return 0; // S_OK
        }
        *ppBuffer = nullptr;
        return static_cast<int32_t>(0x80004005); // E_FAIL
    }

    static int32_t WINAPI Sample_ConvertToContiguousBuffer(void* thisPtr, void** ppBuffer) {
        return Sample_GetBufferByIndex(thisPtr, 0, ppBuffer);
    }

    static int32_t WINAPI Sample_AddBuffer(void* thisPtr, void* pBuffer) {
        if (!pBuffer) return static_cast<int32_t>(0x80004003);
        reinterpret_cast<MFSampleImpl*>(thisPtr)->addBuffer(reinterpret_cast<MFMediaBufferImpl*>(pBuffer));
        return 0; // S_OK
    }
};

// --- 5. CLSID_CColorConvertDMO (IMFTransform) Implementation ---
class MFColorConvertMftImpl {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

    MFColorConvertMftImpl() {
        initVtable();
        lpVtbl = vtable_;
    }

    void* getVtable() { return vtable_; }
    void addRef() { ++refCount_; }
    uint32_t release() {
        uint32_t r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    void setInputSample(MFSampleImpl* sample) {
        if (inputSample_) inputSample_->release();
        inputSample_ = sample;
        if (inputSample_) inputSample_->addRef();
    }

    MFSampleImpl* getInputSample() const { return inputSample_; }

    void setOutputSize(uint32_t w, uint32_t h) {
        width_ = w;
        height_ = h;
    }
    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }

private:
    void* vtable_[32] = { nullptr };
    std::atomic<uint32_t> refCount_{ 1 };
    MFSampleImpl* inputSample_ = nullptr;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;

    void initVtable() {
        for (int i = 0; i < 32; ++i) vtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
        vtable_[0] = reinterpret_cast<void*>(&MFColorConvertMftImpl::QueryInterface);
        vtable_[1] = reinterpret_cast<void*>(&MFColorConvertMftImpl::AddRef);
        vtable_[2] = reinterpret_cast<void*>(&MFColorConvertMftImpl::Release);
        // Slot 15 (offset 120 = 0x78): SetInputType
        vtable_[15] = reinterpret_cast<void*>(&MFColorConvertMftImpl::SetInputType);
        // Slot 16 (offset 128 = 0x80): SetOutputType
        vtable_[16] = reinterpret_cast<void*>(&MFColorConvertMftImpl::SetOutputType);
        // Slot 24 (offset 192 = 0xC0): ProcessInput
        vtable_[24] = reinterpret_cast<void*>(&MFColorConvertMftImpl::ProcessInput);
        // Slot 25 (offset 200 = 0xC8): ProcessOutput
        vtable_[25] = reinterpret_cast<void*>(&MFColorConvertMftImpl::ProcessOutput);
    }

    static int32_t WINAPI QueryInterface(void* thisPtr, const void* riid, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        if (IID_IUnknown.matches(riid) || IID_IMFTransform.matches(riid) || CLSID_CColorConvertDMO.matches(riid)) {
            *ppv = thisPtr;
            reinterpret_cast<MFColorConvertMftImpl*>(thisPtr)->addRef();
            return 0; // S_OK
        }
        *ppv = nullptr;
        return static_cast<int32_t>(0x80004002);
    }

    static uint32_t WINAPI AddRef(void* thisPtr) {
        auto* obj = reinterpret_cast<MFColorConvertMftImpl*>(thisPtr);
        obj->addRef();
        return obj->refCount_.load();
    }

    static uint32_t WINAPI Release(void* thisPtr) {
        return reinterpret_cast<MFColorConvertMftImpl*>(thisPtr)->release();
    }

    static int32_t WINAPI SetInputType(void*, uint32_t, void*, uint32_t) {
        return 0; // S_OK
    }

    static int32_t WINAPI SetOutputType(void* thisPtr, uint32_t, void* pType, uint32_t) {
        if (pType) {
            uint64_t frameSize = 0;
            auto* attrs = reinterpret_cast<MFAttributesImpl*>(pType);
            if (attrs->getUINT64(MF_MT_FRAME_SIZE, frameSize)) {
                uint32_t w = static_cast<uint32_t>(frameSize >> 32);
                uint32_t h = static_cast<uint32_t>(frameSize & 0xFFFFFFFF);
                reinterpret_cast<MFColorConvertMftImpl*>(thisPtr)->setOutputSize(w, h);
                LOGI("CColorConvertDMO::SetOutputType: %ux%u", w, h);
            }
        }
        return 0; // S_OK
    }

    static int32_t WINAPI ProcessInput(void* thisPtr, uint32_t, void* pSample, uint32_t) {
        reinterpret_cast<MFColorConvertMftImpl*>(thisPtr)->setInputSample(reinterpret_cast<MFSampleImpl*>(pSample));
        return 0; // S_OK
    }

    static int32_t WINAPI ProcessOutput(void* thisPtr, uint32_t, uint32_t cOutputBufferCount, void* pOutputSamples, uint32_t* pdwStatus) {
        if (pdwStatus) *pdwStatus = 0;
        if (!pOutputSamples || cOutputBufferCount == 0) return 0;

        auto* mft = reinterpret_cast<MFColorConvertMftImpl*>(thisPtr);
        auto* inSample = mft->getInputSample();
        if (!inSample) return 0;

        // MFT_OUTPUT_DATA_BUFFER layout trên x64 (32 bytes):
        // +0: dwStreamID (u32)
        // +8: pSample (IMFSample*)
        // +16: dwStatus (u32)
        // +24: pEvents (IMFCollection*)
        void** rawStruct = reinterpret_cast<void**>(pOutputSamples);
        auto* dstSample = reinterpret_cast<MFSampleImpl*>(rawStruct[1]);
        if (!dstSample) return 0;

        auto* dstBuf = dstSample->getBuffer(0);
        auto* srcBuf = inSample->getBuffer(0);
        if (dstBuf && srcBuf) {
            uint32_t alignedW = (mft->getWidth() + 15) & ~15;
            uint32_t alignedH = (mft->getHeight() + 15) & ~15;
            if (alignedW == 0) alignedW = 1920;
            if (alignedH == 0) alignedH = 1080;

            const auto& srcData = srcBuf->getData();
            if (!srcData.empty()) {
                sgre::DecodedVideoFrame fakeFrame;
                fakeFrame.pixelData = srcData;
                fakeFrame.width = mft->getWidth();
                fakeFrame.height = mft->getHeight();
                fakeFrame.stride = mft->getWidth();
                fakeFrame.sliceHeight = mft->getHeight();
                fakeFrame.colorFormat = 0; // NV12

                uint8_t* dstPtr = dstBuf->getData().data();
                sgre::WebmDecoder::convertYuvToBottomUpBgrx(fakeFrame, dstPtr, alignedW, alignedH);
                dstBuf->setCurrentLength(alignedW * alignedH * 4);
            }
        }
        return 0; // S_OK
    }
};

// --- 6. IMFMediaSource & IMFSourceReader Implementation ---
class MediaSourceInstance {
public:
    virtual ~MediaSourceInstance() = default;
    virtual bool isLooping() const = 0;
    virtual bool hasVideo() const = 0;
    virtual bool hasAudio() const = 0;
    virtual uint32_t getVideoWidth() const = 0;
    virtual uint32_t getVideoHeight() const = 0;
    virtual int64_t getDuration100ns() const = 0;
    virtual void getAudioFormat(uint32_t& sr, uint32_t& ch) const = 0;
    virtual bool readVideoFrame(sgre::DecodedVideoFrame& frame) = 0;
    virtual bool readAudioFrame(sgre::DecodedAudioFrame& frame) = 0;
    virtual void seekTo(int64_t ptsUs) = 0;
};

// Real Media Source dùng WebmDecoder NDK
class RealWebmMediaSource : public MediaSourceInstance {
public:
    RealWebmMediaSource(void* pByteStream, const std::string& nameHint) {
        isLoop_ = (nameHint.find("titlebg") != std::string::npos || nameHint.find("loop") != std::string::npos);
        decoder_.initFromByteStream(pByteStream, nameHint);
    }

    bool isLooping() const override { return isLoop_; }
    bool hasVideo() const override { return decoder_.hasVideo(); }
    bool hasAudio() const override { return decoder_.hasAudio(); }
    uint32_t getVideoWidth() const override { return decoder_.getVideoWidth(); }
    uint32_t getVideoHeight() const override { return decoder_.getVideoHeight(); }
    int64_t getDuration100ns() const override { return decoder_.getDuration100ns(); }
    void getAudioFormat(uint32_t& sr, uint32_t& ch) const override { decoder_.getAudioFormat(sr, ch); }

    bool readVideoFrame(sgre::DecodedVideoFrame& frame) override {
        bool ok = decoder_.readNextVideoFrame(frame);
        if (!ok && isLoop_) {
            decoder_.seekTo(0);
            ok = decoder_.readNextVideoFrame(frame);
        }
        return ok;
    }

    bool readAudioFrame(sgre::DecodedAudioFrame& frame) override {
        bool ok = decoder_.readNextAudioFrame(frame);
        if (!ok && isLoop_) {
            decoder_.seekTo(0);
            ok = decoder_.readNextAudioFrame(frame);
        }
        return ok;
    }

    void seekTo(int64_t ptsUs) override {
        decoder_.seekTo(ptsUs);
    }

private:
    sgre::WebmDecoder decoder_;
    bool isLoop_ = false;
};

// Fallback Media Source khi thiếu file sg_titlebg.webm trên máy
// Tạo video nền tối kéo dài 235 giây (2,350,000,000 x 100ns) để Main Menu không bao giờ timeout quay lại intro!
class FallbackTitleBgMediaSource : public MediaSourceInstance {
public:
    FallbackTitleBgMediaSource() {
        LOGI("Created FallbackTitleBgMediaSource (235s loop)");
    }

    bool isLooping() const override { return true; }
    bool hasVideo() const override { return true; }
    bool hasAudio() const override { return false; } // Không có audio -> MMoviePlayer tự động tắt audio track, chạy mượt mà
    uint32_t getVideoWidth() const override { return 1920; }
    uint32_t getVideoHeight() const override { return 1080; }
    int64_t getDuration100ns() const override { return 2350000000LL; } // 235 giây
    void getAudioFormat(uint32_t& sr, uint32_t& ch) const override { sr = 48000; ch = 2; }

    bool readVideoFrame(sgre::DecodedVideoFrame& frame) override {
        frame.width = 1920;
        frame.height = 1080;
        frame.stride = 1920;
        frame.sliceHeight = 1080;
        frame.colorFormat = 0; // NV12
        frame.ptsUs = currentPtsUs_;
        frame.durationUs = 16667;
        currentPtsUs_ += 16667;
        if (currentPtsUs_ >= 235000000LL) {
            currentPtsUs_ = 0;
        }

        // Tạo frame màu xám tối nhẹ trong suốt
        size_t ySize = 1920 * 1080;
        size_t uvSize = 1920 * (1080 / 2);
        frame.pixelData.assign(ySize + uvSize, 0);
        memset(frame.pixelData.data(), 16, ySize); // Y = 16 (black)
        memset(frame.pixelData.data() + ySize, 128, uvSize); // UV = 128 (neutral)
        return true;
    }

    bool readAudioFrame(sgre::DecodedAudioFrame&) override {
        return false;
    }

    void seekTo(int64_t ptsUs) override {
        currentPtsUs_ = ptsUs;
    }

private:
    int64_t currentPtsUs_ = 0;
};

// Fallback Cutscene Media Source khi thiếu file video cutscene
// Lập tức báo EOS để kịch bản ADV tiếp tục mượt mà, tuyệt đối không chớp đen màn hình
class FallbackCutsceneMediaSource : public MediaSourceInstance {
public:
    FallbackCutsceneMediaSource() {
        LOGI("Created FallbackCutsceneMediaSource (instant EOS cutscene fallback, no black flash)");
    }

    bool isLooping() const override { return false; }
    bool hasVideo() const override { return false; }
    bool hasAudio() const override { return false; }
    uint32_t getVideoWidth() const override { return 1920; }
    uint32_t getVideoHeight() const override { return 1080; }
    int64_t getDuration100ns() const override { return 0; }
    void getAudioFormat(uint32_t& sr, uint32_t& ch) const override { sr = 48000; ch = 2; }

    bool readVideoFrame(sgre::DecodedVideoFrame&) override {
        return false; // Lập tức EOS, không hiển thị khung hình đen
    }

    bool readAudioFrame(sgre::DecodedAudioFrame&) override {
        return false;
    }

    void seekTo(int64_t) override {}
};

// COM Wrapper cho MediaSource
class MFMediaSourceCOM {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

    explicit MFMediaSourceCOM(std::unique_ptr<MediaSourceInstance> src) : source_(std::move(src)) {
        initVtable();
        lpVtbl = vtable_;
    }

    void* getVtable() { return vtable_; }
    void addRef() { ++refCount_; }
    uint32_t release() {
        uint32_t r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

    MediaSourceInstance* getSource() const { return source_.get(); }

private:
    void* vtable_[16] = { nullptr };
    std::atomic<uint32_t> refCount_{ 1 };
    std::unique_ptr<MediaSourceInstance> source_;

    void initVtable() {
        for (int i = 0; i < 16; ++i) vtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
        vtable_[0] = reinterpret_cast<void*>(&MFMediaSourceCOM::QueryInterface);
        vtable_[1] = reinterpret_cast<void*>(&MFMediaSourceCOM::AddRef);
        vtable_[2] = reinterpret_cast<void*>(&MFMediaSourceCOM::Release);
    }

    static int32_t WINAPI QueryInterface(void* thisPtr, const void*, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        *ppv = thisPtr;
        reinterpret_cast<MFMediaSourceCOM*>(thisPtr)->addRef();
        return 0; // S_OK
    }

    static uint32_t WINAPI AddRef(void* thisPtr) {
        auto* obj = reinterpret_cast<MFMediaSourceCOM*>(thisPtr);
        obj->addRef();
        return obj->refCount_.load();
    }

    static uint32_t WINAPI Release(void* thisPtr) {
        return reinterpret_cast<MFMediaSourceCOM*>(thisPtr)->release();
    }
};

// COM Wrapper cho SourceReader (chuẩn khớp vtable IDA 100%)
class MFSourceReaderCOM {
public:
    void* lpVtbl = nullptr; // Offset 0x00: Bắt buộc để khớp COM interface ABI!

    explicit MFSourceReaderCOM(MFMediaSourceCOM* source) : source_(source) {
        if (source_) source_->addRef();
        initVtable();
        lpVtbl = vtable_;
    }

    ~MFSourceReaderCOM() {
        if (source_) source_->release();
    }

    void* getVtable() { return vtable_; }
    void addRef() { ++refCount_; }
    uint32_t release() {
        uint32_t r = --refCount_;
        if (r == 0) delete this;
        return r;
    }

private:
    void* vtable_[16] = { nullptr };
    std::atomic<uint32_t> refCount_{ 1 };
    MFMediaSourceCOM* source_ = nullptr;

    void initVtable() {
        for (int i = 0; i < 16; ++i) vtable_[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
        vtable_[0] = reinterpret_cast<void*>(&MFSourceReaderCOM::QueryInterface);
        vtable_[1] = reinterpret_cast<void*>(&MFSourceReaderCOM::AddRef);
        vtable_[2] = reinterpret_cast<void*>(&MFSourceReaderCOM::Release);
        // Slot 5 (offset 40 = 0x28): GetNativeMediaType (IDA 0x14038E1F0)
        vtable_[5] = reinterpret_cast<void*>(&MFSourceReaderCOM::GetNativeMediaType);
        // Slot 6 (offset 48 = 0x30): GetCurrentMediaType (IDA 0x14038E1F0, 0x14038EA30)
        vtable_[6] = reinterpret_cast<void*>(&MFSourceReaderCOM::GetCurrentMediaType);
        // Slot 7 (offset 56 = 0x38): SetCurrentMediaType (IDA 0x14038E1F0)
        vtable_[7] = reinterpret_cast<void*>(&MFSourceReaderCOM::SetCurrentMediaType);
        // Slot 8 (offset 64 = 0x40): SetCurrentPosition (IDA 0x14038E7D0)
        vtable_[8] = reinterpret_cast<void*>(&MFSourceReaderCOM::SetCurrentPosition);
        // Slot 9 (offset 72 = 0x48): ReadSample (IDA 0x14038F080, 0x14038EE70)
        vtable_[9] = reinterpret_cast<void*>(&MFSourceReaderCOM::ReadSample);
        // Slot 12 (offset 96 = 0x60): GetPresentationAttribute (IDA 0x14038EA30)
        vtable_[12] = reinterpret_cast<void*>(&MFSourceReaderCOM::GetPresentationAttribute);
    }

    static int32_t WINAPI QueryInterface(void* thisPtr, const void*, void** ppv) {
        if (!ppv) return static_cast<int32_t>(0x80004003);
        *ppv = thisPtr;
        reinterpret_cast<MFSourceReaderCOM*>(thisPtr)->addRef();
        return 0; // S_OK
    }

    static uint32_t WINAPI AddRef(void* thisPtr) {
        auto* obj = reinterpret_cast<MFSourceReaderCOM*>(thisPtr);
        obj->addRef();
        return obj->refCount_.load();
    }

    static uint32_t WINAPI Release(void* thisPtr) {
        return reinterpret_cast<MFSourceReaderCOM*>(thisPtr)->release();
    }

    static int32_t WINAPI GetNativeMediaType(void* thisPtr, uint32_t dwStreamIndex, uint32_t, void** ppMediaType) {
        if (!ppMediaType) return static_cast<int32_t>(0x80004003);
        auto* reader = reinterpret_cast<MFSourceReaderCOM*>(thisPtr);
        auto* src = reader->source_ ? reader->source_->getSource() : nullptr;
        if (!src) return static_cast<int32_t>(0x80004005);

        if (dwStreamIndex == 0xFFFFFFFC) { // MF_SOURCE_READER_FIRST_VIDEO_STREAM
            if (!src->hasVideo()) return static_cast<int32_t>(0xC00D36B3); // MF_E_INVALIDSTREAMNUMBER
            auto* type = new MFMediaTypeImpl();
            type->setGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->setGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            uint64_t fSize = (static_cast<uint64_t>(src->getVideoWidth()) << 32) | src->getVideoHeight();
            type->setUINT64(MF_MT_FRAME_SIZE, fSize);
            *ppMediaType = type;
            return 0; // S_OK
        } else if (dwStreamIndex == 0xFFFFFFFD) { // MF_SOURCE_READER_FIRST_AUDIO_STREAM
            if (!src->hasAudio()) return static_cast<int32_t>(0xC00D36B3); // MF_E_INVALIDSTREAMNUMBER
            auto* type = new MFMediaTypeImpl();
            type->setGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->setGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            uint32_t sr = 48000, ch = 2;
            src->getAudioFormat(sr, ch);
            type->setAudioFormat(sr, ch);
            *ppMediaType = type;
            return 0; // S_OK
        }
        return static_cast<int32_t>(0xC00D36B3); // MF_E_INVALIDSTREAMNUMBER
    }

    static int32_t WINAPI GetCurrentMediaType(void* thisPtr, uint32_t dwStreamIndex, void** ppMediaType) {
        return GetNativeMediaType(thisPtr, dwStreamIndex, 0, ppMediaType);
    }

    static int32_t WINAPI SetCurrentMediaType(void*, uint32_t, uint32_t*, void*) {
        return 0; // S_OK
    }

    static int32_t WINAPI SetCurrentPosition(void* thisPtr, const void*, const void* pvarPosition) {
        auto* reader = reinterpret_cast<MFSourceReaderCOM*>(thisPtr);
        auto* src = reader->source_ ? reader->source_->getSource() : nullptr;
        if (src && pvarPosition) {
            const int64_t* pVal64 = reinterpret_cast<const int64_t*>(reinterpret_cast<const char*>(pvarPosition) + 8);
            int64_t ptsUs = (*pVal64) / 10;
            src->seekTo(ptsUs);
            LOGI("IMFSourceReader::SetCurrentPosition to %lld us (100ns: %lld)", static_cast<long long>(ptsUs), static_cast<long long>(*pVal64));
        }
        return 0; // S_OK
    }

    static int32_t WINAPI ReadSample(
        void* thisPtr, uint32_t dwStreamIndex, uint32_t,
        uint32_t* pdwActualStreamIndex, uint32_t* pdwStreamFlags,
        int64_t* pllTimestamp, void** ppSample
    ) {
        if (pdwActualStreamIndex) *pdwActualStreamIndex = dwStreamIndex;
        if (pdwStreamFlags) *pdwStreamFlags = 0;
        if (pllTimestamp) *pllTimestamp = 0;
        if (ppSample) *ppSample = nullptr;

        auto* reader = reinterpret_cast<MFSourceReaderCOM*>(thisPtr);
        auto* src = reader->source_ ? reader->source_->getSource() : nullptr;
        if (!src) {
            if (pdwStreamFlags) *pdwStreamFlags = 0x00000001; // MF_SOURCE_READERF_ENDOFSTREAM
            return 0;
        }

        if (dwStreamIndex == 0xFFFFFFFC || dwStreamIndex == 0) { // Video (Stream 0 hoặc MF_SOURCE_READER_FIRST_VIDEO_STREAM)
            sgre::DecodedVideoFrame vFrame;
            if (src->readVideoFrame(vFrame)) {
                auto* sample = new MFSampleImpl();
                sample->setSampleTime(vFrame.ptsUs * 10);
                sample->setSampleDuration(vFrame.durationUs * 10);

                auto* buf = new MFMediaBufferImpl(static_cast<uint32_t>(vFrame.pixelData.size()));
                std::memcpy(buf->getData().data(), vFrame.pixelData.data(), vFrame.pixelData.size());
                buf->setCurrentLength(static_cast<uint32_t>(vFrame.pixelData.size()));

                sample->addBuffer(buf);
                buf->release();

                if (pllTimestamp) *pllTimestamp = vFrame.ptsUs * 10;
                *ppSample = sample;
                return 0; // S_OK
            } else {
                if (pdwStreamFlags) *pdwStreamFlags = 0x00000001; // MF_SOURCE_READERF_ENDOFSTREAM
                return 0;
            }
        } else if (dwStreamIndex == 0xFFFFFFFD || dwStreamIndex == 1) { // Audio (Stream 1 hoặc MF_SOURCE_READER_FIRST_AUDIO_STREAM)
            sgre::DecodedAudioFrame aFrame;
            if (src->readAudioFrame(aFrame)) {
                auto* sample = new MFSampleImpl();
                sample->setSampleTime(aFrame.ptsUs * 10);
                sample->setSampleDuration(1000000); // 100ms

                auto* buf = new MFMediaBufferImpl(static_cast<uint32_t>(aFrame.pcmData.size()));
                std::memcpy(buf->getData().data(), aFrame.pcmData.data(), aFrame.pcmData.size());
                buf->setCurrentLength(static_cast<uint32_t>(aFrame.pcmData.size()));

                sample->addBuffer(buf);
                buf->release();

                if (pllTimestamp) *pllTimestamp = aFrame.ptsUs * 10;
                *ppSample = sample;
                return 0; // S_OK
            } else {
                if (pdwStreamFlags) *pdwStreamFlags = 0x00000001; // MF_SOURCE_READERF_ENDOFSTREAM
                return 0;
            }
        }

        if (pdwStreamFlags) *pdwStreamFlags = 0x00000001;
        return 0;
    }

    static int32_t WINAPI GetPresentationAttribute(void* thisPtr, uint32_t, const void* pguidAttribute, void* pvarAttribute) {
        if (!pvarAttribute || !pguidAttribute) return static_cast<int32_t>(0x80004003);
        const auto* attr = reinterpret_cast<const MF_GUID*>(pguidAttribute);
        auto* reader = reinterpret_cast<MFSourceReaderCOM*>(thisPtr);
        auto* src = reader->source_ ? reader->source_->getSource() : nullptr;

        auto* raw = reinterpret_cast<uint8_t*>(pvarAttribute);
        memset(raw, 0, 24);

        if (MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS.matches(attr)) {
            *reinterpret_cast<uint16_t*>(raw) = 19; // VT_UI4
            *reinterpret_cast<uint32_t*>(raw + 8) = 0x7; // MFMEDIASOURCE_CAN_SEEK | CAN_PAUSE
            return 0;
        } else if (MF_PD_DURATION.matches(attr)) {
            *reinterpret_cast<uint16_t*>(raw) = 20; // VT_I8
            int64_t dur = src ? src->getDuration100ns() : 0;
            *reinterpret_cast<int64_t*>(raw + 8) = dur;
            return 0;
        }

        return static_cast<int32_t>(0xC00D36E6); // MF_E_ATTRIBUTENOTFOUND
    }
};

// --- 7. IMFSourceResolver Implementation ---
static int32_t WINAPI SourceResolver_CreateObjectFromByteStream(
    void*, void* pByteStream, const char16_t*, uint32_t, void*, uint32_t* pObjectType, void** ppObject
) {
    if (!pByteStream || !ppObject) return static_cast<int32_t>(0x80004003);

    // Đọc tên file từ std::string ở offset 8 trong MovieReadByteStream (IDA 0x14038EA30, 0x140039730)
    std::string filename;
    const char* strData = reinterpret_cast<const char*>(pByteStream) + 8;
    const uint64_t* strCapacity = reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(pByteStream) + 32);
    if (*strCapacity > 15) {
        const char* const* ptr = reinterpret_cast<const char* const*>(strData);
        if (*ptr) filename = *ptr;
    } else {
        filename = strData;
    }

    // Kiểm tra GetLength
    void** vtable = *reinterpret_cast<void***>(pByteStream);
    using FnGetLength = int32_t (WINAPI *)(void*, uint64_t*);
    auto fnGetLength = reinterpret_cast<FnGetLength>(vtable[4]);
    uint64_t totalLength = 0;
    int32_t hrLen = fnGetLength(pByteStream, &totalLength);

    LOGI("CreateObjectFromByteStream: hint='%s', GetLength=%llu (hr=0x%x)",
         filename.c_str(), static_cast<unsigned long long>(totalLength), hrLen);

    std::unique_ptr<MediaSourceInstance> srcInstance;
    if (totalLength > 0 && hrLen >= 0) {
        auto realSrc = std::make_unique<RealWebmMediaSource>(pByteStream, filename);
        if (realSrc->hasVideo() || realSrc->hasAudio()) {
            srcInstance = std::move(realSrc);
            LOGI("RealWebmMediaSource created successfully for '%s'", filename.c_str());
        }
    }

    if (!srcInstance) {
        // Xử lý fallback an toàn:
        if (filename.find("titlebg") != std::string::npos || filename.find("loop") != std::string::npos) {
            LOGW("Missing titlebg webm data! Activating FallbackTitleBgMediaSource to prevent loop intro timeout");
            srcInstance = std::make_unique<FallbackTitleBgMediaSource>();
        } else {
            // Cutscene thiếu file: kích hoạt FallbackCutsceneMediaSource (100ms dummy video/audio) để game engine nạp thành công và chuyển thẳng sang thoại Visual Novel mà không crash hay ném ngoại lệ
            LOGW("Cutscene video '%s' missing or empty! Activating FallbackCutsceneMediaSource to advance scene cleanly without crash", filename.c_str());
            srcInstance = std::make_unique<FallbackCutsceneMediaSource>();
        }
    }

    if (pObjectType) *pObjectType = 1; // MF_OBJECT_MEDIASOURCE
    auto* mediaSource = new MFMediaSourceCOM(std::move(srcInstance));
    *ppObject = mediaSource;
    return 0; // S_OK
}

static void* gSourceResolverVtable[16] = { nullptr };
static void* gMockSourceResolver = gSourceResolverVtable;

static void initSourceResolverVtableOnce() {
    static bool sInit = false;
    if (sInit) return;
    sInit = true;
    for (int i = 0; i < 16; ++i) gSourceResolverVtable[i] = reinterpret_cast<void*>(&mock_GenericSuccess);
    gSourceResolverVtable[4] = reinterpret_cast<void*>(&SourceResolver_CreateObjectFromByteStream);
}

} // namespace

void MediaFoundationBridge::init() {
    initSourceResolverVtableOnce();
    initDummyDXGIVtableOnce();
}

int32_t WINAPI MediaFoundationBridge::createColorConvertMft(void** ppv) {
    if (!ppv) return static_cast<int32_t>(0x80004003);
    auto* mft = new MFColorConvertMftImpl();
    *ppv = mft;
    LOGI("Created MFColorConvertMftImpl instance: %p", mft);
    return 0; // S_OK
}

extern "C" {

int32_t WINAPI stub_MFStartup(uint32_t version, uint32_t dwFlags) {
    MediaFoundationBridge::init();
    LOGI("MFStartup: version=0x%08x, dwFlags=0x%x -> S_OK", version, dwFlags);
    return 0; // S_OK
}

int32_t WINAPI stub_MFShutdown() {
    LOGI("MFShutdown called -> S_OK");
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateAttributes(void** ppMFAttributes, uint32_t cInitialSize) {
    MediaFoundationBridge::init();
    if (!ppMFAttributes) return static_cast<int32_t>(0x80004003);
    auto* attrs = new MFAttributesImpl();
    *ppMFAttributes = attrs;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateMediaType(void** ppMFType) {
    MediaFoundationBridge::init();
    if (!ppMFType) return static_cast<int32_t>(0x80004003);
    auto* mediaType = new MFMediaTypeImpl();
    *ppMFType = mediaType;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateDXGIDeviceManager(uint32_t* pResetToken, void** ppDeviceManager) {
    MediaFoundationBridge::init();
    if (pResetToken) *pResetToken = 1;
    if (ppDeviceManager) *ppDeviceManager = &gDummyDXGIResource;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateMemoryBuffer(uint32_t cbMaxLength, void** ppBuffer) {
    MediaFoundationBridge::init();
    if (!ppBuffer) return static_cast<int32_t>(0x80004003);
    auto* buf = new MFMediaBufferImpl(cbMaxLength);
    *ppBuffer = buf;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateSample(void** ppSample) {
    MediaFoundationBridge::init();
    if (!ppSample) return static_cast<int32_t>(0x80004003);
    auto* sample = new MFSampleImpl();
    *ppSample = sample;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCalculateImageSize(const void*, uint32_t unWidth, uint32_t unHeight, uint32_t* pcbImageSize) {
    if (pcbImageSize) {
        *pcbImageSize = unWidth * unHeight * 4; // RGB32
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFGetStrideForBitmapInfoHeader(uint32_t, uint32_t dwWidth, int32_t* pStride) {
    if (pStride) {
        *pStride = static_cast<int32_t>(dwWidth * 4);
    }
    return 0; // S_OK
}

int64_t WINAPI stub_MFllMulDiv(int64_t a, int64_t b, int64_t c, int64_t d) {
    if (c == 0) return 0;
    return (a * b + d) / c;
}

int32_t WINAPI stub_MFCreateSourceResolver(void** ppISourceResolver) {
    MediaFoundationBridge::init();
    if (ppISourceResolver) {
        *ppISourceResolver = &gMockSourceResolver;
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateAsyncResult(void*, void*, void*, void** ppOut) {
    if (ppOut) *ppOut = nullptr;
    return 0; // S_OK
}

int32_t WINAPI stub_MFInvokeCallback(void*) {
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateSourceReaderFromMediaSource(void* pMediaSource, void*, void** ppSourceReader) {
    MediaFoundationBridge::init();
    if (!pMediaSource || !ppSourceReader) return static_cast<int32_t>(0x80004003);
    auto* sourceCom = reinterpret_cast<MFMediaSourceCOM*>(pMediaSource);
    auto* reader = new MFSourceReaderCOM(sourceCom);
    *ppSourceReader = reader;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateSourceReaderFromURL(const char16_t*, void*, void** ppSourceReader) {
    if (ppSourceReader) *ppSourceReader = nullptr;
    return static_cast<int32_t>(0x80004005); // E_FAIL
}

} // extern "C"
