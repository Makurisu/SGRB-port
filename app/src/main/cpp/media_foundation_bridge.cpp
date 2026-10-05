#include "media_foundation_bridge.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <codecvt>
#include <locale>

#define LOG_TAG "SGRE_MediaFoundation"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

struct MF_ComGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool matches(const void* ptr) const {
        return ptr && memcmp(this, ptr, sizeof(MF_ComGuid)) == 0;
    }
};

// Vtables riêng biệt cho Media Foundation COM interfaces
static uint64_t gMFAttributesVtable[36] = {0};
static uint64_t gMFMediaTypeVtable[40] = {0};
static uint64_t gMFSourceResolverVtable[16] = {0};
static uint64_t gMFSourceReaderVtable[16] = {0};
static uint64_t gMFDXGIDeviceManagerVtable[16] = {0};
static uint64_t gMFMediaBufferVtable[16] = {0};
static uint64_t gMFSampleVtable[48] = {0};
static uint64_t gMFMediaSourceVtable[16] = {0};

struct MF_COM_Mock {
    void* vtable;
};

static MF_COM_Mock gMockMFAttributes = { gMFAttributesVtable };
static MF_COM_Mock gMockMFMediaType = { gMFMediaTypeVtable };
static MF_COM_Mock gMockMFSourceResolver = { gMFSourceResolverVtable };
static MF_COM_Mock gMockMFSourceReader = { gMFSourceReaderVtable };
static MF_COM_Mock gMockMFDXGIDeviceManager = { gMFDXGIDeviceManagerVtable };
static MF_COM_Mock gMockMFMediaBuffer = { gMFMediaBufferVtable };
static MF_COM_Mock gMockMFSample = { gMFSampleVtable };
static MF_COM_Mock gMockMFMediaSource = { gMFMediaSourceVtable };

static int64_t WINAPI mock_MF_QueryInterface(void* thisPtr, const void*, void** ppvObject) {
    if (!ppvObject) return 0x80004003; // E_POINTER
    *ppvObject = thisPtr;
    return 0; // S_OK
}

static uint32_t WINAPI mock_MF_AddRef(void*) { return 1; }
static uint32_t WINAPI mock_MF_Release(void*) { return 1; }
static int64_t WINAPI mock_MF_Generic(void*, void*, void*, void*, void*, void*) { return 0; }

// IMFSourceResolver::CreateObjectFromURL
static int64_t WINAPI mock_SourceResolver_CreateObjectFromURL(
    void*, const char16_t* pwszURL, uint32_t dwFlags, void* pProps, uint32_t* pObjectType, void** ppObject
) {
    std::string urlUtf8 = "<null>";
    if (pwszURL) {
        std::u16string u16(pwszURL);
        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
        urlUtf8 = convert.to_bytes(u16);
    }
    LOGI("IMFSourceResolver::CreateObjectFromURL: url='%s', flags=0x%x, pProps=%p",
         urlUtf8.c_str(), dwFlags, pProps);

    if (pObjectType) {
        *pObjectType = 1; // MF_OBJECT_MEDIASOURCE
    }
    if (ppObject) {
        *ppObject = &gMockMFMediaSource;
    }
    return 0; // S_OK
}

// IMFSourceReader::ReadSample
static int64_t WINAPI mock_SourceReader_ReadSample(
    void*, uint32_t dwStreamIndex, uint32_t dwControlFlags,
    uint32_t* pdwActualStreamIndex, uint32_t* pdwStreamFlags,
    int64_t* pllTimestamp, void** ppSample
) {
    static uint32_t sReadSampleCount = 0;
    if (++sReadSampleCount % 60 == 1) {
        LOGI("IMFSourceReader::ReadSample: StreamIdx=%u, ControlFlags=0x%x (call #%u)",
             dwStreamIndex, dwControlFlags, sReadSampleCount);
    }

    if (pdwActualStreamIndex) *pdwActualStreamIndex = dwStreamIndex;
    if (pdwStreamFlags) {
        // Gán MF_SOURCE_READERF_ENDOFSTREAM = 0x00000001 để game biết stream video đã kết thúc
        // hoặc sẵn sàng chuyển sang giao diện chính không bị block
        *pdwStreamFlags = 0x00000001;
    }
    if (pllTimestamp) *pllTimestamp = 0;
    if (ppSample) *ppSample = nullptr;

    return 0; // S_OK
}

// IMFSourceReader::SetCurrentMediaType
static int64_t WINAPI mock_SourceReader_SetCurrentMediaType(void*, uint32_t dwStreamIndex, uint32_t*, void* pMediaType) {
    LOGI("IMFSourceReader::SetCurrentMediaType: StreamIdx=%u, MediaType=%p", dwStreamIndex, pMediaType);
    return 0; // S_OK
}

// IMFSourceReader::GetCurrentMediaType
static int64_t WINAPI mock_SourceReader_GetCurrentMediaType(void*, uint32_t dwStreamIndex, void** ppMediaType) {
    LOGI("IMFSourceReader::GetCurrentMediaType: StreamIdx=%u", dwStreamIndex);
    if (ppMediaType) *ppMediaType = &gMockMFMediaType;
    return 0; // S_OK
}

static void initMFVtablesOnce() {
    static bool sInitialized = false;
    if (sInitialized) return;
    sInitialized = true;

    // 1. Generic stubs cho toàn bộ vtable slots
    for (int i = 0; i < 36; ++i) gMFAttributesVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
    for (int i = 0; i < 40; ++i) gMFMediaTypeVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
    for (int i = 0; i < 16; ++i) {
        gMFSourceResolverVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
        gMFSourceReaderVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
        gMFDXGIDeviceManagerVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
        gMFMediaBufferVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
        gMFMediaSourceVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);
    }
    for (int i = 0; i < 48; ++i) gMFSampleVtable[i] = reinterpret_cast<uint64_t>(&mock_MF_Generic);

    // 2. IUnknown base slots (0, 1, 2)
    uint64_t* allTables[] = {
        gMFAttributesVtable, gMFMediaTypeVtable, gMFSourceResolverVtable,
        gMFSourceReaderVtable, gMFDXGIDeviceManagerVtable, gMFMediaBufferVtable,
        gMFSampleVtable, gMFMediaSourceVtable
    };
    for (auto* tbl : allTables) {
        tbl[0] = reinterpret_cast<uint64_t>(&mock_MF_QueryInterface);
        tbl[1] = reinterpret_cast<uint64_t>(&mock_MF_AddRef);
        tbl[2] = reinterpret_cast<uint64_t>(&mock_MF_Release);
    }

    // 3. IMFSourceResolver slots
    gMFSourceResolverVtable[3] = reinterpret_cast<uint64_t>(&mock_SourceResolver_CreateObjectFromURL);
    gMFSourceResolverVtable[4] = reinterpret_cast<uint64_t>(&mock_SourceResolver_CreateObjectFromURL); // CreateObjectFromByteStream

    // 4. IMFSourceReader slots
    gMFSourceReaderVtable[5] = reinterpret_cast<uint64_t>(&mock_SourceReader_GetCurrentMediaType);
    gMFSourceReaderVtable[6] = reinterpret_cast<uint64_t>(&mock_SourceReader_SetCurrentMediaType);
    gMFSourceReaderVtable[10] = reinterpret_cast<uint64_t>(&mock_SourceReader_ReadSample);
}

void MediaFoundationBridge::init() {
    initMFVtablesOnce();
}

extern "C" {

int32_t WINAPI stub_MFStartup(uint32_t version, uint32_t dwFlags) {
    initMFVtablesOnce();
    LOGI("MFStartup: version=0x%08x, dwFlags=0x%x -> S_OK", version, dwFlags);
    return 0; // S_OK
}

int32_t WINAPI stub_MFShutdown() {
    LOGI("MFShutdown called -> S_OK");
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateAttributes(void** ppMFAttributes, uint32_t cInitialSize) {
    initMFVtablesOnce();
    LOGI("MFCreateAttributes: cInitialSize=%u", cInitialSize);
    if (ppMFAttributes) {
        *ppMFAttributes = &gMockMFAttributes;
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateMediaType(void** ppMFType) {
    initMFVtablesOnce();
    LOGI("MFCreateMediaType called");
    if (ppMFType) {
        *ppMFType = &gMockMFMediaType;
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateDXGIDeviceManager(uint32_t* pResetToken, void** ppDeviceManager) {
    initMFVtablesOnce();
    LOGI("MFCreateDXGIDeviceManager called");
    if (pResetToken) *pResetToken = 1;
    if (ppDeviceManager) *ppDeviceManager = &gMockMFDXGIDeviceManager;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateMemoryBuffer(uint32_t cbMaxLength, void** ppBuffer) {
    initMFVtablesOnce();
    LOGI("MFCreateMemoryBuffer: cbMaxLength=%u", cbMaxLength);
    if (ppBuffer) *ppBuffer = &gMockMFMediaBuffer;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateSample(void** ppSample) {
    initMFVtablesOnce();
    LOGI("MFCreateSample called");
    if (ppSample) *ppSample = &gMockMFSample;
    return 0; // S_OK
}

int32_t WINAPI stub_MFCalculateImageSize(const void* guidSubtype, uint32_t unWidth, uint32_t unHeight, uint32_t* pcbImageSize) {
    LOGI("MFCalculateImageSize: %ux%u", unWidth, unHeight);
    if (pcbImageSize) {
        *pcbImageSize = unWidth * unHeight * 4; // RGBA32
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFGetStrideForBitmapInfoHeader(uint32_t format, uint32_t dwWidth, int32_t* pStride) {
    LOGI("MFGetStrideForBitmapInfoHeader: format=%u, width=%u", format, dwWidth);
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
    initMFVtablesOnce();
    LOGI("MFCreateSourceResolver called");
    if (ppISourceResolver) {
        *ppISourceResolver = &gMockMFSourceResolver;
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

int32_t WINAPI stub_MFCreateSourceReaderFromMediaSource(void* pMediaSource, void* pAttributes, void** ppSourceReader) {
    initMFVtablesOnce();
    LOGI("MFCreateSourceReaderFromMediaSource: MediaSource=%p, Attributes=%p", pMediaSource, pAttributes);
    if (ppSourceReader) {
        *ppSourceReader = &gMockMFSourceReader;
    }
    return 0; // S_OK
}

int32_t WINAPI stub_MFCreateSourceReaderFromURL(const char16_t* pwszURL, void* pAttributes, void** ppSourceReader) {
    initMFVtablesOnce();
    std::string urlUtf8 = "<null>";
    if (pwszURL) {
        std::u16string u16(pwszURL);
        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
        urlUtf8 = convert.to_bytes(u16);
    }
    LOGI("MFCreateSourceReaderFromURL: url='%s', Attributes=%p", urlUtf8.c_str(), pAttributes);
    if (ppSourceReader) {
        *ppSourceReader = &gMockMFSourceReader;
    }
    return 0; // S_OK
}

}
