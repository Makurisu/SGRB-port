#pragma once
#include <cstdint>

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

class MediaFoundationBridge {
public:
    static void init();
    static int32_t WINAPI createColorConvertMft(void** ppv);
};

extern "C" {
int32_t WINAPI stub_MFStartup(uint32_t version, uint32_t dwFlags);
int32_t WINAPI stub_MFShutdown();
int32_t WINAPI stub_MFCreateAttributes(void** ppMFAttributes, uint32_t cInitialSize);
int32_t WINAPI stub_MFCreateMediaType(void** ppMFType);
int32_t WINAPI stub_MFCreateDXGIDeviceManager(uint32_t* pResetToken, void** ppDeviceManager);
int32_t WINAPI stub_MFCreateMemoryBuffer(uint32_t cbMaxLength, void** ppBuffer);
int32_t WINAPI stub_MFCreateSample(void** ppSample);
int32_t WINAPI stub_MFCalculateImageSize(const void* guidSubtype, uint32_t unWidth, uint32_t unHeight, uint32_t* pcbImageSize);
int32_t WINAPI stub_MFGetStrideForBitmapInfoHeader(uint32_t format, uint32_t dwWidth, int32_t* pStride);
int64_t WINAPI stub_MFllMulDiv(int64_t a, int64_t b, int64_t c, int64_t d);
int32_t WINAPI stub_MFCreateSourceResolver(void** ppISourceResolver);
int32_t WINAPI stub_MFCreateAsyncResult(void* punkObject, void* pCallback, void* punkState, void** ppOut);
int32_t WINAPI stub_MFInvokeCallback(void* pAsyncResult);
int32_t WINAPI stub_MFCreateSourceReaderFromMediaSource(void* pMediaSource, void* pAttributes, void** ppSourceReader);
int32_t WINAPI stub_MFCreateSourceReaderFromURL(const char16_t* pwszURL, void* pAttributes, void** ppSourceReader);
}
