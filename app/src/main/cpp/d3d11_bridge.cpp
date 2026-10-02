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

// Slot 6: IDXGIObject::GetParent(this, riid, ppParent)
static int64_t WINAPI mock_COM_GetParent(void* thisPtr, const void*, void** ppParent) {
    LOGI("IDXGI GetParent called on %p", thisPtr);
    if (ppParent) {
        *ppParent = thisPtr;
    }
    return 0; // S_OK
}

// Slot 7: IDXGIDevice::GetAdapter(this, ppAdapter)
static int64_t WINAPI mock_COM_GetAdapter(void* thisPtr, void** ppAdapter) {
    LOGI("IDXGIDevice GetAdapter called on %p", thisPtr);
    if (ppAdapter) {
        *ppAdapter = thisPtr;
    }
    return 0; // S_OK
}

// Slot 3: ID3D11Device::CreateBuffer(this, pDesc, pInitialData, ppBuffer)
static int64_t WINAPI mock_COM_CreateBuffer(void* thisPtr, const void*, const void*, void** ppBuffer) {
    if (ppBuffer) *ppBuffer = thisPtr;
    return 0; // S_OK
}

// Slot 5: ID3D11Device::CreateTexture2D(this, pDesc, pInitialData, ppTexture2D)
static int64_t WINAPI mock_COM_CreateTexture2D(void* thisPtr, const void*, const void*, void** ppTexture2D) {
    if (ppTexture2D) *ppTexture2D = thisPtr;
    return 0; // S_OK
}

// Slot 8/9/11/12/15: Views & Shaders (CreateShaderResourceView, CreateRenderTargetView, etc.)
static int64_t WINAPI mock_COM_CreateView(void* thisPtr, void*, const void*, void** ppView) {
    if (ppView) *ppView = thisPtr;
    return 0; // S_OK
}

// Slot 10: IDXGIFactory::CreateSwapChain(this, pDevice, pDesc, ppSwapChain)
static int64_t WINAPI mock_COM_CreateSwapChain(void* thisPtr, void*, void*, void** ppSwapChain) {
    LOGI("IDXGIFactory CreateSwapChain called on %p", thisPtr);
    if (ppSwapChain) {
        *ppSwapChain = thisPtr;
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

    // COM IUnknown slots (0, 1, 2)
    gDummyDeviceVtable[0] = reinterpret_cast<uint64_t>(&mock_COM_QueryInterface);
    gDummyDeviceVtable[1] = reinterpret_cast<uint64_t>(&mock_COM_AddRef);
    gDummyDeviceVtable[2] = reinterpret_cast<uint64_t>(&mock_COM_Release);

    // ID3D11Device & DXGI Interface slots
    gDummyDeviceVtable[3] = reinterpret_cast<uint64_t>(&mock_COM_CreateBuffer);
    gDummyDeviceVtable[5] = reinterpret_cast<uint64_t>(&mock_COM_CreateTexture2D);
    gDummyDeviceVtable[6] = reinterpret_cast<uint64_t>(&mock_COM_GetParent); // IDXGIObject::GetParent
    gDummyDeviceVtable[7] = reinterpret_cast<uint64_t>(&mock_COM_GetAdapter); // IDXGIDevice::GetAdapter
    gDummyDeviceVtable[8] = reinterpret_cast<uint64_t>(&mock_COM_CreateView); // CreateShaderResourceView
    gDummyDeviceVtable[9] = reinterpret_cast<uint64_t>(&mock_COM_CreateView); // CreateRenderTargetView / GetBuffer
    gDummyDeviceVtable[10] = reinterpret_cast<uint64_t>(&mock_COM_CreateSwapChain); // CreateSwapChain
    gDummyDeviceVtable[11] = reinterpret_cast<uint64_t>(&mock_COM_CreateView); // CreateInputLayout
    gDummyDeviceVtable[12] = reinterpret_cast<uint64_t>(&mock_COM_CreateView); // CreateVertexShader
    gDummyDeviceVtable[15] = reinterpret_cast<uint64_t>(&mock_COM_CreateView); // CreatePixelShader

    // DeviceContext slots
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
