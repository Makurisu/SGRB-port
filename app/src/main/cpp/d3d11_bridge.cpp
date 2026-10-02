#include "d3d11_bridge.h"
#include <android/log.h>
#include <cstdlib>

#define LOG_TAG "SGRE_D3D11Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static uint64_t gDummyDeviceVtable[128] = {0};
static uint64_t gDummyContextVtable[128] = {0};

static SGRE_D3D11_DeviceMock gMockDevice = { gDummyDeviceVtable };
static SGRE_D3D11_DeviceContextMock gMockContext = { gDummyContextVtable };

static int64_t WINAPI mock_COM_QueryInterface(void* thisPtr, const void* /* riid */, void** ppvObject) {
    LOGI("D3D11 QueryInterface called on %p", thisPtr);
    if (ppvObject) {
        *ppvObject = thisPtr;
    }
    return 0; // S_OK
}

static uint32_t WINAPI mock_COM_AddRef(void*) { return 1; }
static uint32_t WINAPI mock_COM_Release(void*) { return 1; }

static int64_t WINAPI mock_COM_GetParent(void* thisPtr, const void*, void** ppParent) {
    LOGI("IDXGI GetParent called on %p", thisPtr);
    if (ppParent) {
        *ppParent = thisPtr;
    }
    return 0; // S_OK
}

static int64_t WINAPI mock_COM_Generic(void*, void*, void*, void*, void*, void*, void*, void*) {
    return 0; // S_OK
}

static void initVtablesOnce() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    for (int i = 0; i < 128; ++i) {
        gDummyDeviceVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gDummyContextVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
    }

    // COM IUnknown slots
    gDummyDeviceVtable[0] = reinterpret_cast<uint64_t>(&mock_COM_QueryInterface);
    gDummyDeviceVtable[1] = reinterpret_cast<uint64_t>(&mock_COM_AddRef);
    gDummyDeviceVtable[2] = reinterpret_cast<uint64_t>(&mock_COM_Release);
    // IDXGIObject GetParent slot 6
    gDummyDeviceVtable[6] = reinterpret_cast<uint64_t>(&mock_COM_GetParent);

    gDummyContextVtable[0] = reinterpret_cast<uint64_t>(&mock_COM_QueryInterface);
    gDummyContextVtable[1] = reinterpret_cast<uint64_t>(&mock_COM_AddRef);
    gDummyContextVtable[2] = reinterpret_cast<uint64_t>(&mock_COM_Release);
}

int32_t D3D11Bridge::createDevice(
    void* /* pAdapter */,
    int32_t driverType,
    void* /* software */,
    uint32_t flags,
    const void* /* pFeatureLevels */,
    uint32_t featureLevels,
    uint32_t sdkVersion,
    void** ppDevice,
    void* pFeatureLevel,
    void** ppImmediateContext
) {
    initVtablesOnce();
    LOGI("D3D11CreateDevice intercepted: DriverType=%d, Flags=0x%X, FeatureLevelsCount=%u, SDKVersion=%u",
         driverType, flags, featureLevels, sdkVersion);

    if (pFeatureLevel != nullptr) {
        // Trả về D3D_FEATURE_LEVEL_11_0 = 0xb000
        *reinterpret_cast<uint32_t*>(pFeatureLevel) = 0xb000;
    }

    if (ppDevice != nullptr) {
        *ppDevice = &gMockDevice;
        LOGI("-> Đã cấp phát Mock D3D11 Device: %p", *ppDevice);
    }

    if (ppImmediateContext != nullptr) {
        *ppImmediateContext = &gMockContext;
        LOGI("-> Đã cấp phát Mock D3D11 DeviceContext: %p", *ppImmediateContext);
    }

    // S_OK = 0
    LOGI("D3D11CreateDevice -> SUCCESS (S_OK = 0)");
    return 0;
}

extern "C" {
int32_t WINAPI D3D11CreateDevice(
    void* pAdapter,
    int32_t driverType,
    void* software,
    uint32_t flags,
    const void* pFeatureLevels,
    uint32_t featureLevels,
    uint32_t sdkVersion,
    void** ppDevice,
    void* pFeatureLevel,
    void** ppImmediateContext
) {
    return D3D11Bridge::createDevice(
        pAdapter, driverType, software, flags, pFeatureLevels, featureLevels,
        sdkVersion, ppDevice, pFeatureLevel, ppImmediateContext
    );
}
}
