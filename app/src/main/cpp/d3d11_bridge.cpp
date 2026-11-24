#include "d3d11_bridge.h"
#include "dxbc_reflector.h"
#include "resource_registry.h"
#include "win32_bridge.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#define LOG_TAG "SGRE_D3D11Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

struct ComGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool matches(const void* ptr) const {
        return ptr && memcmp(this, ptr, sizeof(ComGuid)) == 0;
    }
};

// Định nghĩa các GUID chuẩn DirectX 11 / DXGI được game query
static const ComGuid IID_IUnknown_Val          = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const ComGuid IID_ID3D11Device_Val      = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };
static const ComGuid IID_ID3D11Device1_Val     = { 0xa04462db, 0x9756, 0x4b6e, { 0xaf, 0xdb, 0xc0, 0xf7, 0x09, 0xfa, 0xb4, 0xee } };
static const ComGuid IID_ID3D11Context_Val     = { 0xc0bfa96c, 0xe089, 0x44fb, { 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda } };
static const ComGuid IID_ID3D11Context1_Val    = { 0xbb2c6015, 0x62ca, 0x45ee, { 0x91, 0xff, 0xb0, 0x4c, 0x0e, 0x34, 0xac, 0x85 } };
static const ComGuid IID_ID3D11Multithread_Val = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
static const ComGuid IID_IDXGIDevice_Val       = { 0x54ec77fa, 0x1377, 0x4e66, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const ComGuid IID_IDXGIDevice1_Val      = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
static const ComGuid IID_IDXGIAdapter_Val      = { 0x2411e0e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0x00 } };
static const ComGuid IID_IDXGIAdapter1_Val     = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };
static const ComGuid IID_IDXGIFactory_Val      = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const ComGuid IID_IDXGIFactory1_Val     = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const ComGuid IID_IDXGIFactory2_Val     = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const ComGuid IID_IDXGISwapChain_Val    = { 0x310d36a0, 0xd837, 0x44da, { 0xaa, 0x5f, 0xe0, 0x50, 0xe2, 0x56, 0xf2, 0x0d } };
static const ComGuid IID_IDXGISwapChain1_Val   = { 0x790a45f7, 0x0d42, 0x4876, { 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa } };
static const ComGuid IID_ID3D11Texture2D_Val   = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const ComGuid IID_ID3D11ShaderReflection_Val = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

// Vtables riêng biệt cho từng COM interface để ngăn chặn tuyệt đối xung đột slot/tham số
static uint64_t gDeviceVtable[128] = {0};
static uint64_t gContextVtable[128] = {0};
static uint64_t gMultithreadVtable[16] = {0};
static uint64_t gDXGIDeviceVtable[32] = {0};
static uint64_t gDXGIAdapterVtable[32] = {0};
static uint64_t gDXGIFactoryVtable[32] = {0};
static uint64_t gSwapChainVtable[32] = {0};
static uint64_t gTexture2DVtable[32] = {0};
static uint64_t gViewVtable[32] = {0};
static uint64_t gGenericResourceVtable[32] = {0};
static uint64_t gShaderReflectionVtable[32] = {0};
static uint64_t gShaderReflectionConstantBufferVtable[16] = {0};
static uint64_t gShaderReflectionVariableVtable[16] = {0};
static uint64_t gShaderReflectionTypeVtable[16] = {0};

// Mock COM Instances
static SGRE_COM_Mock gMockDevice = { gDeviceVtable };
static SGRE_COM_Mock gMockContext = { gContextVtable };
static SGRE_COM_Mock gMockMultithread = { gMultithreadVtable };
static SGRE_COM_Mock gMockDXGIDevice = { gDXGIDeviceVtable };
static SGRE_COM_Mock gMockDXGIAdapter = { gDXGIAdapterVtable };
static SGRE_COM_Mock gMockDXGIFactory = { gDXGIFactoryVtable };
static SGRE_COM_Mock gMockSwapChain = { gSwapChainVtable };
static SGRE_COM_Mock gMockTexture2D = { gTexture2DVtable };
static SGRE_COM_Mock gMockGenericResource = { gGenericResourceVtable };
static SGRE_COM_Mock gMockShaderReflection = { gShaderReflectionVtable };
static SGRE_COM_Mock gMockShaderConstantBuffer = { gShaderReflectionConstantBufferVtable };
static SGRE_COM_Mock gMockShaderVariable = { gShaderReflectionVariableVtable };
static SGRE_COM_Mock gMockShaderType = { gShaderReflectionTypeVtable };

struct MockShader {
    void* vtable;
    uint32_t id;
    bool isPixelShader;
};
static MockShader sPixelShaders[2080];
static MockShader sVertexShaders[16];
static uint32_t sCurrentPixelShaderId = 0;
static uint32_t sCurrentVertexShaderId = 0;
static void* sBoundConstantBuffersPS[16] = {nullptr};
static void* sBoundConstantBuffersVS[16] = {nullptr};

// Bảng bitmask tra cứu O(1) các Pixel Shader sử dụng Texture Slot 1 (t1 Alpha Mask)
// Được trích xuất từ 2077 DXBC bytecode của M2 Engine (chunk RDEF: bind_point == 1)
static const uint8_t sShaderUsesT1Bits[260] = {
    0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
    0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x49,
    0x92, 0x24, 0x49, 0x00,
};

static inline bool isShaderUsingT1(uint32_t shaderId) {
    if (shaderId < 2080) {
        return (sShaderUsesT1Bits[shaderId / 8] & (1 << (shaderId % 8))) != 0;
    }
    return false;
}

static inline bool isShaderUsingT2(uint32_t shaderId) {
    return (shaderId >= 900 && shaderId < 2040);
}

// Vùng nhớ đệm an toàn cho ID3D11DeviceContext::Map
static uint8_t sMappedScratchMemory[16 * 1024 * 1024]; // 16 MB

// Quản lý tài nguyên CPU
static sgre::ResourceRegistry sRegistry(sgre::kDefaultCpuBudgetBytes);

static uint32_t WINAPI mock_COM_AddRef(void* thisPtr);

// COM IUnknown methods
static int64_t WINAPI mock_COM_QueryInterface(void* thisPtr, const void* riid, void** ppvObject) {
    if (!ppvObject) return 0x80004003; // E_POINTER

    const auto* g = reinterpret_cast<const ComGuid*>(riid);
    if (g) {
        LOGI("COM QueryInterface on %p for GUID: %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             thisPtr, g->data1, g->data2, g->data3,
             g->data4[0], g->data4[1], g->data4[2], g->data4[3],
             g->data4[4], g->data4[5], g->data4[6], g->data4[7]);

        if (IID_ID3D11Multithread_Val.matches(riid)) {
            *ppvObject = &gMockMultithread;
            LOGI("-> Phản hồi Mock ID3D11Multithread: %p", *ppvObject);
            return 0; // S_OK
        }
        if (IID_IDXGIDevice_Val.matches(riid) || IID_IDXGIDevice1_Val.matches(riid)) {
            *ppvObject = &gMockDXGIDevice;
            LOGI("-> Phản hồi Mock IDXGIDevice1: %p", *ppvObject);
            return 0;
        }
        if (IID_IDXGIAdapter_Val.matches(riid) || IID_IDXGIAdapter1_Val.matches(riid)) {
            *ppvObject = &gMockDXGIAdapter;
            LOGI("-> Phản hồi Mock IDXGIAdapter: %p", *ppvObject);
            return 0;
        }
        if (IID_IDXGIFactory_Val.matches(riid) || IID_IDXGIFactory1_Val.matches(riid) || IID_IDXGIFactory2_Val.matches(riid)) {
            *ppvObject = &gMockDXGIFactory;
            LOGI("-> Phản hồi Mock IDXGIFactory2: %p", *ppvObject);
            return 0;
        }
        if (IID_IDXGISwapChain_Val.matches(riid) || IID_IDXGISwapChain1_Val.matches(riid)) {
            *ppvObject = &gMockSwapChain;
            LOGI("-> Phản hồi Mock IDXGISwapChain: %p", *ppvObject);
            return 0;
        }
        if (IID_ID3D11Texture2D_Val.matches(riid)) {
            if (thisPtr != nullptr && sRegistry.contains(thisPtr)) {
                *ppvObject = thisPtr;
                mock_COM_AddRef(thisPtr);
                LOGI("-> Phản hồi Registered ID3D11Texture2D: %p", *ppvObject);
                return 0;
            }
            *ppvObject = &gMockTexture2D;
            LOGI("-> Phản hồi Mock ID3D11Texture2D: %p", *ppvObject);
            return 0;
        }
        if (IID_ID3D11Context_Val.matches(riid) || IID_ID3D11Context1_Val.matches(riid)) {
            *ppvObject = &gMockContext;
            LOGI("-> Phản hồi Mock ID3D11DeviceContext1: %p", *ppvObject);
            return 0;
        }
        if (IID_ID3D11Device_Val.matches(riid) || IID_ID3D11Device1_Val.matches(riid)) {
            *ppvObject = &gMockDevice;
            LOGI("-> Phản hồi Mock ID3D11Device1: %p", *ppvObject);
            return 0;
        }
        if (IID_ID3D11ShaderReflection_Val.matches(riid)) {
            *ppvObject = &gMockShaderReflection;
            LOGI("-> Phản hồi Mock ID3D11ShaderReflection: %p", *ppvObject);
            return 0;
        }
    }

    // Mặc định trả về chính con trỏ đối tượng hiện tại và tăng refcount theo chuẩn COM
    *ppvObject = thisPtr;
    mock_COM_AddRef(thisPtr);
    return 0; // S_OK
}

static uint32_t WINAPI mock_COM_AddRef(void* thisPtr) {
    if (!thisPtr) return 1;
    int32_t ref = sRegistry.addRef(thisPtr);
    return ref > 0 ? static_cast<uint32_t>(ref) : 1;
}
static std::mutex sDeferredMutex;
static std::vector<const void*> sDeferredReleaseList;

static uint32_t WINAPI mock_COM_Release(void* thisPtr) {
    if (!thisPtr) return 0;
    int32_t ref = sRegistry.release(thisPtr);
    if (ref == 0) {
        // Chỉ trì hoãn giải phóng heap pointer khi ref count của sRegistry thực sự về 0
        bool isStaticShader = (thisPtr >= &sPixelShaders[0] && thisPtr < &sPixelShaders[2080]) ||
                              (thisPtr >= &sVertexShaders[0] && thisPtr < &sVertexShaders[16]);
        if (!isStaticShader &&
            thisPtr != &gMockDevice &&
            thisPtr != &gMockContext &&
            thisPtr != &gMockMultithread &&
            thisPtr != &gMockDXGIDevice &&
            thisPtr != &gMockDXGIAdapter &&
            thisPtr != &gMockDXGIFactory &&
            thisPtr != &gMockSwapChain &&
            thisPtr != &gMockTexture2D &&
            thisPtr != &gMockGenericResource &&
            thisPtr != &gMockShaderReflection &&
            thisPtr != &gMockShaderConstantBuffer &&
            thisPtr != &gMockShaderVariable &&
            thisPtr != &gMockShaderType) {
            std::lock_guard<std::mutex> lock(sDeferredMutex);
            sDeferredReleaseList.push_back(thisPtr);
        }
        return 0;
    }
    return ref > 0 ? static_cast<uint32_t>(ref) : 1;
}

// Fallback method tuyệt đối an toàn: KHÔNG dereference bất kỳ con trỏ nào
static int64_t WINAPI mock_COM_Generic(void*, void*, void*, void*, void*, void*, void*, void*) {
    return 0; // S_OK
}

// ID3D11Multithread methods
static void WINAPI mock_MT_Enter(void*) {}
static void WINAPI mock_MT_Leave(void*) {}
static int32_t WINAPI mock_MT_SetMultithreadProtected(void*, int32_t bMTProtect) {
    LOGI("ID3D11Multithread::SetMultithreadProtected(%d)", bMTProtect);
    return 1; // Trả về trạng thái trước đó = TRUE
}
static int32_t WINAPI mock_MT_GetMultithreadProtected(void*) {
    return 1;
}

// IDXGIObject::GetParent
static int64_t WINAPI mock_COM_GetParent(void*, const void*, void** ppParent) {
    if (ppParent) {
        *ppParent = &gMockDXGIFactory;
    }
    return 0; // S_OK
}

// IDXGIDevice::GetAdapter
static int64_t WINAPI mock_DXGI_GetAdapter(void*, void** ppAdapter) {
    if (ppAdapter) {
        *ppAdapter = &gMockDXGIAdapter;
    }
    return 0; // S_OK
}

// IDXGIDevice1 methods
static int64_t WINAPI mock_DXGI_SetMaximumFrameLatency(void*, uint32_t) { return 0; }
static int64_t WINAPI mock_DXGI_GetMaximumFrameLatency(void*, uint32_t* pMaxLatency) {
    if (pMaxLatency) *pMaxLatency = 1;
    return 0;
}

// Các cấu trúc mô phỏng DXGI SwapChain và Direct3D 11 Texture
struct MockDXGIRational {
    uint32_t numerator;
    uint32_t denominator;
};

struct MockDXGIModeDesc {
    uint32_t width;
    uint32_t height;
    MockDXGIRational refreshRate;
    uint32_t format;
    uint32_t scanlineOrdering;
    uint32_t scaling;
};

struct MockDXGISampleDesc {
    uint32_t count;
    uint32_t quality;
};

struct MockSwapChainDesc {
    MockDXGIModeDesc bufferDesc;     // 0x00 - 0x1C (28 bytes)
    MockDXGISampleDesc sampleDesc;   // 0x1C - 0x24 (8 bytes)
    uint32_t bufferUsage;            // 0x24 - 0x28 (4 bytes)
    uint32_t bufferCount;            // 0x28 - 0x2C (4 bytes) -> offset 0x28 (40)
    void* outputWindow;              // 0x30 - 0x38 (8 bytes)
    int32_t windowed;                // 0x38 - 0x3C (4 bytes)
    uint32_t swapEffect;             // 0x3C - 0x40 (4 bytes)
    uint32_t flags;                  // 0x40 - 0x44 (4 bytes)
};

struct MockSwapChainDesc1 {
    uint32_t width;                  // 0x00
    uint32_t height;                 // 0x04
    uint32_t format;                 // 0x08
    int32_t stereo;                  // 0x0C
    MockDXGISampleDesc sampleDesc;   // 0x10 - 0x18
    uint32_t bufferUsage;            // 0x18
    uint32_t bufferCount;            // 0x1C
    uint32_t scaling;                // 0x20
    uint32_t swapEffect;             // 0x24
    uint32_t alphaMode;              // 0x28
    uint32_t flags;                  // 0x2C
};

struct MockSwapChainFullscreenDesc {
    MockDXGIRational refreshRate;    // 0x00
    uint32_t scanlineOrdering;       // 0x08
    uint32_t scaling;                // 0x0C
    int32_t windowed;                // 0x10
};

struct MockTexture2DDesc {
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t format;
    uint32_t sampleCount;
    uint32_t sampleQuality;
    uint32_t usage;
    uint32_t bindFlags;
    uint32_t cpuAccessFlags;
    uint32_t miscFlags;
};

// Thông số SwapChain và Texture2D hiện tại
static uint32_t gCurrentWidth = 1920;
static uint32_t gCurrentHeight = 1080;
static uint32_t gCurrentFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
static uint32_t gCurrentBufferCount = 2; // Tối thiểu 2 buffers để tránh FPE_INTDIV

// Đồng bộ hoá khung hình giữa Game Thread (D3D11) và Android Render Thread (EGL)
static std::mutex sFrameMutex;
static std::condition_variable sFrameCond;
static std::atomic<uint32_t> sTotalBuffersCreated{0};
static std::atomic<uint32_t> sTotalTexturesCreated{0};
static std::atomic<uint32_t> sTotalShadersCreated{0};
static std::atomic<uint32_t> sTotalVertexShadersCreated{0};
static std::atomic<uint32_t> sTotalPixelShadersCreated{0};
static std::atomic<uint32_t> sTotalDrawIndexedCalls{0};
static std::atomic<uint32_t> sTotalDrawCalls{0};
static RenderFrameState sCurrentFrameState = { {0.0f, 0.0f, 0.0f, 1.0f}, 0, 1920, 1080, 0, 0, 0, 0, 0, 0, 0 };
static bool sNewFrameAvailable = false;
static std::condition_variable sFrameAcknowledgeCond;
static std::atomic<bool> sFramePendingAck{false};
static std::condition_variable sSpriteConsumedCond;
static std::atomic<bool> sSpritePendingConsume{false};
static std::atomic<bool> sWakeRequested{false};
static std::atomic<bool> sMaskNeedsReset{true};

// Hằng số D3D11 chuẩn dùng nội bộ
constexpr uint32_t kD3d11UsageStaging = 3;
constexpr uint32_t kD3d11BindRenderTarget = 0x20;
constexpr uint32_t kD3d11CpuAccessRead = 0x20000;
constexpr uint32_t kD3d11MapRead = 1;
constexpr uint32_t kD3d11MapReadWrite = 3;
constexpr uint8_t kCmdCopySubresource = 3;
constexpr uint32_t kGpuReadbackTimeoutMs = 1500;
constexpr uint32_t kPreuploadMinPixels = 128u * 128u;

// Hàng đợi yêu cầu pre-upload texture tĩnh (đặc biệt BC7 atlas) sang Shared EGL Context
static std::mutex sPreuploadMutex;
static std::condition_variable sPreuploadCond;
static std::vector<TextureUploadRequest> sPreuploadQueue;
static std::atomic<bool> sPreuploadWake{false};

// Đồng bộ flush giữa frame và readback GPU -> CPU cho Staging Texture
static std::mutex sFlushMutex;
static std::condition_variable sFlushCond;
static bool sFlushPending = false;
static bool sFlushDone = false;
static bool sFlushSucceeded = false;
static std::atomic<bool> sRenderLoopActive{false};
static GpuFlushRequest sPendingFlushRequest;

struct ActiveMapInfo {
    uint32_t mapType = 0;
    bool readFromGpu = false;
};
static std::mutex sMapTrackingMutex;
static std::unordered_map<const void*, ActiveMapInfo> sActiveMappedModes;

// IDXGIFactory methods
static int64_t WINAPI mock_Factory_MakeWindowAssociation(void*, void*, uint32_t) {
    LOGI("IDXGIFactory::MakeWindowAssociation called");
    return 0;
}

static int64_t WINAPI mock_Factory_CreateSwapChain(void*, void*, const void* pDesc, void** ppSwapChain) {
    LOGI("IDXGIFactory::CreateSwapChain called");
    if (pDesc) {
        const auto* desc = reinterpret_cast<const MockSwapChainDesc*>(pDesc);
        if (desc->bufferDesc.width > 0) gCurrentWidth = desc->bufferDesc.width;
        if (desc->bufferDesc.height > 0) gCurrentHeight = desc->bufferDesc.height;
        if (desc->bufferDesc.format > 0) gCurrentFormat = desc->bufferDesc.format;
        if (desc->bufferCount > 0) gCurrentBufferCount = desc->bufferCount;
        LOGI("  SwapChainDesc: %ux%u, format=%u, bufferCount=%u",
             gCurrentWidth, gCurrentHeight, gCurrentFormat, gCurrentBufferCount);
    }
    if (ppSwapChain) {
        *ppSwapChain = &gMockSwapChain;
    }
    return 0;
}

static int64_t WINAPI mock_Factory_CreateSwapChainForHwnd(
    void*, void*, void* hWnd, const void* pDesc, const void*, void*, void** ppSwapChain
) {
    LOGI("IDXGIFactory2::CreateSwapChainForHwnd called with hWnd=%p", hWnd);
    if (pDesc) {
        const auto* desc1 = reinterpret_cast<const MockSwapChainDesc1*>(pDesc);
        if (desc1->width > 0) gCurrentWidth = desc1->width;
        if (desc1->height > 0) gCurrentHeight = desc1->height;
        if (desc1->format > 0) gCurrentFormat = desc1->format;
        if (desc1->bufferCount > 0) gCurrentBufferCount = desc1->bufferCount;
        LOGI("  SwapChainDesc1: %ux%u, format=%u, bufferCount=%u",
             gCurrentWidth, gCurrentHeight, gCurrentFormat, gCurrentBufferCount);
    }
    if (ppSwapChain) {
        *ppSwapChain = &gMockSwapChain;
    }
    return 0;
}

static std::mutex sSpriteMutex;
static std::vector<SpriteDrawCommand> sActiveDrawingSprites;
static std::vector<SpriteDrawCommand> sReadyFrameSprites;
static std::vector<MeshDrawCommand> sActiveDrawingMeshes;
static std::vector<MeshDrawCommand> sReadyFrameMeshes;
static const void* sCurrentVertexBuffer = nullptr;
static uint32_t sCurrentVertexStride = 20; // 5 floats/fields = 20 bytes: pos.x, pos.y, color, uv.u, uv.v
static uint32_t sCurrentVertexOffset = 0;
static const void* sCurrentIndexBuffer = nullptr;
static uint32_t sCurrentIndexFormat = 57; // DXGI_FORMAT_R16_UINT = 57
static uint32_t sCurrentIndexOffset = 0;
static const void* sActiveShaderResourceView = nullptr;
static const void* sActiveTextureResource = nullptr;
static const void* sBoundShaderResourceViews[8] = {nullptr};
static const void* sBoundTextureResources[8] = {nullptr};
static const void* sLastUpdatedTextureResource = nullptr;
static const void* sLastOffscreenMaskResource = nullptr;
static uint32_t sCurrentPrimitiveTopology = 4; // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4

// IDXGISwapChain methods
static int64_t WINAPI mock_SwapChain_GetDevice(void*, const void*, void** ppDevice) {
    if (ppDevice) *ppDevice = &gMockDevice;
    return 0;
}

static void ThrottlePresentCadence(uint32_t syncInterval) {
    static auto sLastPresentTime = std::chrono::steady_clock::time_point{};
    static bool sInitialized = false;

    auto now = std::chrono::steady_clock::now();
    if (!sInitialized) {
        sInitialized = true;
        sLastPresentTime = now;
        return;
    }

    // syncInterval = 0: unthrottled; syncInterval >= 1: pacing theo nhịp 60 FPS (16,666,666 ns)
    uint32_t intervals = (syncInterval > 0) ? syncInterval : 1;
    auto targetInterval = std::chrono::nanoseconds(1000000000LL * intervals / 60);

    auto elapsed = now - sLastPresentTime;
    if (elapsed < targetInterval) {
        auto waitDuration = targetInterval - elapsed;
        std::this_thread::sleep_for(waitDuration);
        sLastPresentTime = std::chrono::steady_clock::now();
    } else {
        sLastPresentTime = now;
    }
}


static int64_t WINAPI mock_SwapChain_Present(void*, uint32_t syncInterval, uint32_t flags) {
    sMaskNeedsReset.store(true);
    Win32Bridge_OnFramePresented();
    static uint32_t frameCount = 0;
    frameCount++;
    if (frameCount % 60 == 1) {
        LOGI("IDXGISwapChain::Present: frame %u, syncInterval=%u, flags=0x%x", frameCount, syncInterval, flags);
    }

    // Pipelined Concurrent Frame Submission:
    // 1. Chờ frame trước được Render Thread swap commands nếu có sự cạnh tranh hàng đợi
    {
        std::unique_lock<std::mutex> lock(sFrameMutex);
        if (sSpritePendingConsume.load()) {
            sSpriteConsumedCond.wait_for(lock, std::chrono::milliseconds(10), []{
                return !sSpritePendingConsume.load();
            });
        }
    }

    // 2. Double-buffering Pipelining: Giới hạn tối đa 1 frame in-flight để khoá đồng pha theo VSync của Android SurfaceFlinger
    {
        std::unique_lock<std::mutex> lock(sFrameMutex);
        if (sFramePendingAck.load()) {
            sFrameAcknowledgeCond.wait_for(lock, std::chrono::milliseconds(10), []{
                return !sFramePendingAck.load();
            });
        }
    }

    {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sCurrentFrameState.frameIndex = frameCount;
        sCurrentFrameState.width = gCurrentWidth;
        sCurrentFrameState.height = gCurrentHeight;
        sCurrentFrameState.drawCalls = sTotalDrawCalls.load();
        sCurrentFrameState.indexDraws = sTotalDrawIndexedCalls.load();
        sCurrentFrameState.texturesCreated = sTotalTexturesCreated.load();
        sCurrentFrameState.shadersCreated = sTotalShadersCreated.load();
        sCurrentFrameState.buffersCreated = sTotalBuffersCreated.load();
        const auto stats = sRegistry.stats();
        sCurrentFrameState.cpuBytes = stats.cpuBytes;
        sCurrentFrameState.droppedResources = stats.droppedResources;
        sNewFrameAvailable = true;
        sFramePendingAck.store(true);
    }
    {
        std::lock_guard<std::mutex> sLock(sSpriteMutex);
        sReadyFrameSprites = std::move(sActiveDrawingSprites);
        sActiveDrawingSprites.clear();
        sReadyFrameMeshes = std::move(sActiveDrawingMeshes);
        sActiveDrawingMeshes.clear();
    }
    sSpritePendingConsume.store(true);
    sFrameCond.notify_one();
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetBuffer(void*, uint32_t bufferIndex, const void*, void** ppSurface) {
    LOGI("IDXGISwapChain::GetBuffer called for buffer %u", bufferIndex);
    if (ppSurface) {
        *ppSurface = &gMockTexture2D;
    }
    return 0;
}

static int64_t WINAPI mock_SwapChain_SetFullscreenState(void*, int32_t fullscreen, void*) {
    LOGI("IDXGISwapChain::SetFullscreenState(%d)", fullscreen);
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetFullscreenState(void*, int32_t* pFullscreen, void** ppTarget) {
    if (pFullscreen) *pFullscreen = 0; // Android luôn windowed trong View
    if (ppTarget) *ppTarget = nullptr;
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetDesc(void*, void* pDesc) {
    LOGI("IDXGISwapChain::GetDesc called, returning %ux%u, format=%u, bufferCount=%u",
         gCurrentWidth, gCurrentHeight, gCurrentFormat, gCurrentBufferCount);
    if (pDesc) {
        auto* desc = reinterpret_cast<MockSwapChainDesc*>(pDesc);
        memset(desc, 0, sizeof(MockSwapChainDesc));
        desc->bufferDesc.width = gCurrentWidth;
        desc->bufferDesc.height = gCurrentHeight;
        desc->bufferDesc.refreshRate.numerator = 60;
        desc->bufferDesc.refreshRate.denominator = 1;
        desc->bufferDesc.format = gCurrentFormat;
        desc->bufferDesc.scanlineOrdering = 0;
        desc->bufferDesc.scaling = 0;
        desc->sampleDesc.count = 1;
        desc->sampleDesc.quality = 0;
        desc->bufferUsage = 0x20 | 0x40; // DXGI_USAGE_RENDER_TARGET_OUTPUT
        desc->bufferCount = gCurrentBufferCount; // BẮT BUỘC >= 1 để tránh lỗi chia cho 0 FPE_INTDIV
        desc->outputWindow = nullptr;
        desc->windowed = 1;
        desc->swapEffect = 0; // DXGI_SWAP_EFFECT_DISCARD
        desc->flags = 0;
    }
    return 0;
}

static int64_t WINAPI mock_SwapChain_ResizeBuffers(void*, uint32_t bufferCount, uint32_t width, uint32_t height, uint32_t newFormat, uint32_t) {
    LOGI("IDXGISwapChain::ResizeBuffers called: %ux%u, bufferCount=%u, format=%u", width, height, bufferCount, newFormat);
    if (width > 0) gCurrentWidth = width;
    if (height > 0) gCurrentHeight = height;
    if (bufferCount > 0) gCurrentBufferCount = bufferCount;
    if (newFormat > 0) gCurrentFormat = newFormat;
    return 0;
}

static int64_t WINAPI mock_SwapChain_ResizeTarget(void*, const void*) {
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetContainingOutput(void*, void** ppOutput) {
    if (ppOutput) *ppOutput = nullptr;
    return 0x887A0002; // DXGI_ERROR_NOT_FOUND
}

static int64_t WINAPI mock_SwapChain_GetFrameStatistics(void*, void*) {
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetLastPresentCount(void*, uint32_t* pCount) {
    if (pCount) *pCount = 0;
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetDesc1(void*, void* pDesc) {
    LOGI("IDXGISwapChain1::GetDesc1 called");
    if (pDesc) {
        auto* desc1 = reinterpret_cast<MockSwapChainDesc1*>(pDesc);
        memset(desc1, 0, sizeof(MockSwapChainDesc1));
        desc1->width = gCurrentWidth;
        desc1->height = gCurrentHeight;
        desc1->format = gCurrentFormat;
        desc1->stereo = 0;
        desc1->sampleDesc.count = 1;
        desc1->sampleDesc.quality = 0;
        desc1->bufferUsage = 0x20 | 0x40;
        desc1->bufferCount = gCurrentBufferCount;
        desc1->scaling = 0;
        desc1->swapEffect = 0;
        desc1->alphaMode = 0;
        desc1->flags = 0;
    }
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetFullscreenDesc(void*, void* pDesc) {
    if (pDesc) {
        auto* fDesc = reinterpret_cast<MockSwapChainFullscreenDesc*>(pDesc);
        memset(fDesc, 0, sizeof(MockSwapChainFullscreenDesc));
        fDesc->refreshRate.numerator = 60;
        fDesc->refreshRate.denominator = 1;
        fDesc->windowed = 1;
    }
    return 0;
}

static int64_t WINAPI mock_SwapChain_GetHwnd(void*, void** pHwnd) {
    if (pHwnd) *pHwnd = nullptr;
    return 0;
}

static int64_t WINAPI mock_SwapChain_Present1(void*, uint32_t syncInterval, uint32_t flags, const void*) {
    sMaskNeedsReset.store(true);
    Win32Bridge_OnFramePresented();
    static uint32_t frameCount1 = 0;
    frameCount1++;
    if (frameCount1 % 60 == 1) {
        LOGI("IDXGISwapChain1::Present1: frame %u, syncInterval=%u, flags=0x%x", frameCount1, syncInterval, flags);
    }

    // Pipelined Concurrent Frame Submission:
    // 1. Chờ frame trước được Render Thread swap commands nếu có sự cạnh tranh hàng đợi
    {
        std::unique_lock<std::mutex> lock(sFrameMutex);
        if (sSpritePendingConsume.load()) {
            sSpriteConsumedCond.wait_for(lock, std::chrono::milliseconds(10), []{
                return !sSpritePendingConsume.load();
            });
        }
    }

    // 2. Double-buffering Pipelining: Giới hạn tối đa 1 frame in-flight để khoá đồng pha theo VSync của Android SurfaceFlinger
    {
        std::unique_lock<std::mutex> lock(sFrameMutex);
        if (sFramePendingAck.load()) {
            sFrameAcknowledgeCond.wait_for(lock, std::chrono::milliseconds(10), []{
                return !sFramePendingAck.load();
            });
        }
    }

    {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sCurrentFrameState.frameIndex = frameCount1;
        sCurrentFrameState.width = gCurrentWidth;
        sCurrentFrameState.height = gCurrentHeight;
        sCurrentFrameState.drawCalls = sTotalDrawCalls.load();
        sCurrentFrameState.indexDraws = sTotalDrawIndexedCalls.load();
        sCurrentFrameState.texturesCreated = sTotalTexturesCreated.load();
        sCurrentFrameState.shadersCreated = sTotalShadersCreated.load();
        sCurrentFrameState.buffersCreated = sTotalBuffersCreated.load();
        const auto stats = sRegistry.stats();
        sCurrentFrameState.cpuBytes = stats.cpuBytes;
        sCurrentFrameState.droppedResources = stats.droppedResources;
        sNewFrameAvailable = true;
        sFramePendingAck.store(true);
    }
    {
        std::lock_guard<std::mutex> sLock(sSpriteMutex);
        sReadyFrameSprites = std::move(sActiveDrawingSprites);
        sActiveDrawingSprites.clear();
        sReadyFrameMeshes = std::move(sActiveDrawingMeshes);
        sActiveDrawingMeshes.clear();

        // Telemetry theo dõi dòng chảy biến thiên animation định lượng
        static auto sLastPresentTime = std::chrono::steady_clock::now();
        auto nowTime = std::chrono::steady_clock::now();
        float deltaMs = std::chrono::duration<float, std::milli>(nowTime - sLastPresentTime).count();
        sLastPresentTime = nowTime;

        static uint32_t sAnimTrackCounter = 0;
        ++sAnimTrackCounter;
        bool shouldLogAnim = !sReadyFrameMeshes.empty() &&
            (sAnimTrackCounter <= 5 || sAnimTrackCounter % 600 == 1);
        if (shouldLogAnim) {
            size_t totalIndices = 0;
            for (const auto& m : sReadyFrameMeshes) {
                totalIndices += m.indexData.size();
            }
            LOGI("[AnimStream #%u] dt=%.2fms | meshes=%zu, total_tris=%zu",
                 frameCount1, deltaMs, sReadyFrameMeshes.size(), totalIndices / 3);
            for (size_t mi = 0; mi < sReadyFrameMeshes.size() && mi < 128; ++mi) {
                const auto& m = sReadyFrameMeshes[mi];
                float vx0 = (m.vertexData.size() >= 11) ? m.vertexData[0] : 0.0f;
                float vy0 = (m.vertexData.size() >= 11) ? m.vertexData[1] : 0.0f;
                float u0  = (m.vertexData.size() >= 11) ? m.vertexData[3] : 0.0f;
                float v0  = (m.vertexData.size() >= 11) ? m.vertexData[4] : 0.0f;

                float vx1 = (m.vertexData.size() >= 22) ? m.vertexData[11] : 0.0f;
                float vy1 = (m.vertexData.size() >= 22) ? m.vertexData[12] : 0.0f;
                float u1  = (m.vertexData.size() >= 22) ? m.vertexData[14] : 0.0f;
                float v1  = (m.vertexData.size() >= 22) ? m.vertexData[15] : 0.0f;

                float vx2 = (m.vertexData.size() >= 33) ? m.vertexData[22] : 0.0f;
                float vy2 = (m.vertexData.size() >= 33) ? m.vertexData[23] : 0.0f;
                float va0 = (m.vertexData.size() >= 11) ? m.vertexData[10] : 1.0f;

                LOGI("  -> Mesh[%zu]: clr=%u, rt=%p, tex=%p (%ux%u), tris=%zu, vs=%u, ps=%u, alphaTex=%p, destTex=%p (%ux%u), blend=%u(%x,%x,%x|%x,%x,%x), cb0=[%.3f, %.3f, %.3f, %.3f], vA0=%.3f | V0=(%.2f, %.2f) UV0=(%.4f, %.4f) | V1=(%.2f, %.2f) UV1=(%.4f, %.4f) | V2=(%.2f, %.2f)",
                     mi, m.isClearCommand, m.renderTargetHandle, m.textureHandle, m.textureWidth, m.textureHeight,
                     m.indexData.size() / 3, m.vsShaderId, m.shaderId, m.alphaTextureHandle,
                     m.destTextureHandle, m.destTextureWidth, m.destTextureHeight,
                     m.blendEnable, m.srcBlend, m.destBlend, m.blendOp, m.srcBlendAlpha, m.destBlendAlpha, m.blendOpAlpha,
                     m.cb0[0], m.cb0[1], m.cb0[2], m.cb0[3], va0,
                     vx0, vy0, u0, v0, vx1, vy1, u1, v1, vx2, vy2);
            }
        }
    }
    sSpritePendingConsume.store(true);
    sFrameCond.notify_one();
    return 0;
}

// ID3D11Resource::GetType
static void WINAPI mock_Resource_GetType(void*, uint32_t* pDim) {
    if (pDim) *pDim = 4; // D3D11_RESOURCE_DIMENSION_TEXTURE2D
}

// ID3D11DeviceChild::GetDevice
static void WINAPI mock_Resource_GetDevice(void*, void** ppDevice) {
    if (ppDevice) *ppDevice = &gMockDevice;
}

// ID3D11Texture2D::GetDesc
static void WINAPI mock_Texture2D_GetDesc(void* thisPtr, void* pDesc) {
    if (!pDesc) return;
    auto* desc = reinterpret_cast<MockTexture2DDesc*>(pDesc);
    sgre::TextureDesc tDesc{};
    if (thisPtr != nullptr && thisPtr != &gMockTexture2D && sRegistry.getTextureDesc(thisPtr, tDesc)) {
        desc->width = tDesc.width;
        desc->height = tDesc.height;
        desc->mipLevels = tDesc.mipLevels;
        desc->arraySize = tDesc.arraySize;
        desc->format = tDesc.format;
        desc->sampleCount = tDesc.sampleCount > 0 ? tDesc.sampleCount : 1;
        desc->sampleQuality = tDesc.sampleQuality;
        desc->usage = tDesc.usage;
        desc->bindFlags = tDesc.bindFlags;
        desc->cpuAccessFlags = tDesc.cpuAccessFlags;
        desc->miscFlags = tDesc.miscFlags;
        return;
    }
    desc->width = gCurrentWidth;
    desc->height = gCurrentHeight;
    desc->mipLevels = 1;
    desc->arraySize = 1;
    desc->format = gCurrentFormat;
    desc->sampleCount = 1;
    desc->sampleQuality = 0;
    desc->usage = 0; // D3D11_USAGE_DEFAULT
    desc->bindFlags = 0x20 | 0x08; // D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
    desc->cpuAccessFlags = 0;
    desc->miscFlags = 0;
}

struct MockBufferDesc {
    uint32_t byteWidth;
    uint32_t usage;
    uint32_t bindFlags;
    uint32_t cpuAccessFlags;
    uint32_t miscFlags;
    uint32_t structureByteStride;
};

struct MockSubresourceData {
    const void* pSysMem;
    uint32_t sysMemPitch;
    uint32_t sysMemSlicePitch;
};

struct MockViewport {
    float topLeftX;
    float topLeftY;
    float width;
    float height;
    float minDepth;
    float maxDepth;
};

struct MockRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct MockDepthStencilOpDesc {
    uint32_t stencilFailOp;
    uint32_t stencilDepthFailOp;
    uint32_t stencilPassOp;
    uint32_t stencilFunc;
};

struct MockDepthStencilDesc {
    int32_t depthEnable;
    uint32_t depthWriteMask;
    uint32_t depthFunc;
    int32_t stencilEnable;
    uint8_t stencilReadMask;
    uint8_t stencilWriteMask;
    MockDepthStencilOpDesc frontFace;
    MockDepthStencilOpDesc backFace;
};

struct MockDepthStencilStateCOM : public SGRE_COM_Mock {
    MockDepthStencilDesc desc;
};

struct MockRenderTargetBlendDesc {
    int32_t blendEnable;
    uint32_t srcBlend;
    uint32_t destBlend;
    uint32_t blendOp;
    uint32_t srcBlendAlpha;
    uint32_t destBlendAlpha;
    uint32_t blendOpAlpha;
    uint8_t renderTargetWriteMask; // 0x1=R, 0x2=G, 0x4=B, 0x8=A, 0xF=ALL, 0=NONE
};

struct MockBlendDesc {
    int32_t alphaToCoverageEnable;
    int32_t independentBlendEnable;
    MockRenderTargetBlendDesc renderTarget[8];
};

struct MockBlendStateCOM : public SGRE_COM_Mock {
    MockBlendDesc desc;
};

static const MockDepthStencilStateCOM* sCurrentDepthStencilState = nullptr;
static uint32_t sCurrentStencilRef = 0;
static const MockBlendStateCOM* sCurrentBlendState = nullptr;
static float sCurrentBlendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static MockViewport sCurrentViewport = {0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f};

static inline uint32_t d3dComparisonToGl(uint32_t d3dFunc) {
    switch (d3dFunc) {
        case 1: return 0x0200; // GL_NEVER
        case 2: return 0x0201; // GL_LESS
        case 3: return 0x0202; // GL_EQUAL
        case 4: return 0x0203; // GL_LEQUAL
        case 5: return 0x0204; // GL_GREATER
        case 6: return 0x0205; // GL_NOTEQUAL
        case 7: return 0x0206; // GL_GEQUAL
        case 8: return 0x0207; // GL_ALWAYS
        default: return 0x0207; // GL_ALWAYS
    }
}

static inline uint32_t d3dStencilOpToGl(uint32_t d3dOp) {
    switch (d3dOp) {
        case 1: return 0x1E00; // GL_KEEP
        case 2: return 0;      // GL_ZERO
        case 3: return 0x1E01; // GL_REPLACE
        case 4: return 0x1E02; // GL_INCR
        case 5: return 0x1E03; // GL_DECR
        case 6: return 0x150A; // GL_INVERT
        case 7: return 0x8507; // GL_INCR_WRAP
        case 8: return 0x8508; // GL_DECR_WRAP
        default: return 0x1E00; // GL_KEEP
    }
}

static inline uint32_t d3dBlendToGl(uint32_t d3dBlend) {
    switch (d3dBlend) {
        case 1: return 0;          // D3D11_BLEND_ZERO -> GL_ZERO
        case 2: return 1;          // D3D11_BLEND_ONE -> GL_ONE
        case 3: return 0x0300;     // D3D11_BLEND_SRC_COLOR -> GL_SRC_COLOR
        case 4: return 0x0301;     // D3D11_BLEND_INV_SRC_COLOR -> GL_ONE_MINUS_SRC_COLOR
        case 5: return 0x0302;     // D3D11_BLEND_SRC_ALPHA -> GL_SRC_ALPHA
        case 6: return 0x0303;     // D3D11_BLEND_INV_SRC_ALPHA -> GL_ONE_MINUS_SRC_ALPHA
        case 7: return 0x0304;     // D3D11_BLEND_DEST_ALPHA -> GL_DST_ALPHA
        case 8: return 0x0305;     // D3D11_BLEND_INV_DEST_ALPHA -> GL_ONE_MINUS_DST_ALPHA
        case 9: return 0x0306;     // D3D11_BLEND_DEST_COLOR -> GL_DST_COLOR
        case 10: return 0x0307;    // D3D11_BLEND_INV_DEST_COLOR -> GL_ONE_MINUS_DST_COLOR
        case 11: return 0x0308;    // D3D11_BLEND_SRC_ALPHA_SAT -> GL_SRC_ALPHA_SATURATE
        case 14: return 0x8001;    // D3D11_BLEND_BLEND_FACTOR -> GL_CONSTANT_COLOR
        case 15: return 0x8002;    // D3D11_BLEND_INV_BLEND_FACTOR -> GL_ONE_MINUS_CONSTANT_COLOR
        default: return 0x0302;    // GL_SRC_ALPHA
    }
}

static inline uint32_t d3dBlendOpToGl(uint32_t d3dOp) {
    switch (d3dOp) {
        case 1: return 0x8006;     // D3D11_BLEND_OP_ADD -> GL_FUNC_ADD
        case 2: return 0x800A;     // D3D11_BLEND_OP_SUBTRACT -> GL_FUNC_SUBTRACT
        case 3: return 0x800B;     // D3D11_BLEND_OP_REV_SUBTRACT -> GL_FUNC_REVERSE_SUBTRACT
        case 4: return 0x8007;     // D3D11_BLEND_OP_MIN -> GL_MIN
        case 5: return 0x8008;     // D3D11_BLEND_OP_MAX -> GL_MAX
        default: return 0x8006;    // GL_FUNC_ADD
    }
}

struct MockRasterizerDesc {
    int32_t fillMode;
    int32_t cullMode;
    int32_t frontCounterClockwise;
    int32_t depthBias;
    float depthBiasClamp;
    float slopeScaledDepthBias;
    int32_t depthClipEnable;
    int32_t scissorEnable;
    int32_t multisampleEnable;
    int32_t antialiasedLineEnable;
};

struct MockRasterizerStateCOM : public SGRE_COM_Mock {
    MockRasterizerDesc desc;
};

static const MockRasterizerStateCOM* sCurrentRasterizerState = nullptr;
static MockRect sCurrentScissorRect = {0, 0, 1920, 1080};
static bool sHasScissorRect = false;

// ID3D11Device methods
static int64_t WINAPI mock_Device_CreateBuffer(void*, const void* pDesc, const void* pInitialData, void** ppBuffer) {
    sTotalBuffersCreated++;
    auto* mockBuf = new SGRE_COM_Mock{ gGenericResourceVtable };
    if (pDesc) {
        const auto* desc = reinterpret_cast<const MockBufferDesc*>(pDesc);
        static uint32_t sCreateBufferLogCount = 0;
        if (++sCreateBufferLogCount <= 10) {
            LOGI("ID3D11Device::CreateBuffer #%u: mockBuf=%p, ByteWidth=%u, Usage=%u, BindFlags=0x%x, InitialData=%p",
                 sCreateBufferLogCount, mockBuf, desc->byteWidth, desc->usage, desc->bindFlags, pInitialData);
        }

        sgre::BufferDesc bufDesc{};
        bufDesc.byteWidth = desc->byteWidth;
        bufDesc.usage = desc->usage;
        bufDesc.bindFlags = desc->bindFlags;
        bufDesc.cpuAccessFlags = desc->cpuAccessFlags;
        bufDesc.miscFlags = desc->miscFlags;
        bufDesc.structureByteStride = desc->structureByteStride;

        const void* initMem = nullptr;
        if (pInitialData) {
            const auto* subData = reinterpret_cast<const MockSubresourceData*>(pInitialData);
            initMem = subData->pSysMem;
            if (desc->byteWidth == 80 && initMem != nullptr) {
                const auto* f = reinterpret_cast<const float*>(initMem);
                static uint32_t sBuf80Count = 0;
                if (++sBuf80Count <= 5) {
                    LOGI("CreateBuffer[80] #%u: V0=[%.2f, %.2f, %.2f, %.2f, %.2f] V1=[%.2f, %.2f, %.2f, %.2f, %.2f] V2=[%.2f, %.2f, %.2f, %.2f, %.2f] V3=[%.2f, %.2f, %.2f, %.2f, %.2f]",
                         sBuf80Count,
                         f[0], f[1], f[2], f[3], f[4],
                         f[5], f[6], f[7], f[8], f[9],
                         f[10], f[11], f[12], f[13], f[14],
                         f[15], f[16], f[17], f[18], f[19]);
                }
            }
        }
        sRegistry.registerBuffer(mockBuf, bufDesc, initMem);
    }
    if (ppBuffer) *ppBuffer = mockBuf;
    return 0;
}

static int64_t WINAPI mock_Device_CreateTexture2D(void*, const void* pDesc, const void* pInitialData, void** ppTexture2D) {
    static std::atomic<uint64_t> sNextTextureGen{1};
    sTotalTexturesCreated++;
    auto* mockTex = new SGRE_COM_Mock{ gTexture2DVtable };
    if (pDesc) {
        const auto* desc = reinterpret_cast<const MockTexture2DDesc*>(pDesc);
        static uint32_t sCreateTexLog = 0;
        uint32_t logIdx = ++sCreateTexLog;
        if (logIdx <= 25 || desc->format == sgre::kDxgiBc7Unorm ||
            desc->format == sgre::kDxgiBc7UnormSrgb || logIdx % 120 == 1) {
            LOGI("ID3D11Device::CreateTexture2D: %ux%u, Mips=%u, Array=%u, Format=%u, BindFlags=0x%x, InitialData=%p",
                 desc->width, desc->height, desc->mipLevels, desc->arraySize, desc->format, desc->bindFlags, pInitialData);
        }

        sgre::TextureDesc texDesc{};
        texDesc.width = desc->width;
        texDesc.height = desc->height;
        texDesc.mipLevels = desc->mipLevels;
        texDesc.arraySize = desc->arraySize;
        texDesc.format = desc->format;
        texDesc.sampleCount = desc->sampleCount;
        texDesc.sampleQuality = desc->sampleQuality;
        texDesc.usage = desc->usage;
        texDesc.bindFlags = desc->bindFlags;
        texDesc.cpuAccessFlags = desc->cpuAccessFlags;
        texDesc.miscFlags = desc->miscFlags;
        texDesc.generationId = sNextTextureGen.fetch_add(1);

        const void* initMem = nullptr;
        uint32_t initPitch = 0;
        if (pInitialData) {
            const auto* subData = reinterpret_cast<const MockSubresourceData*>(pInitialData);
            initMem = subData->pSysMem;
            initPitch = subData->sysMemPitch;
        }
        auto tRegStart = std::chrono::steady_clock::now();
        bool registered = sRegistry.registerTexture(mockTex, texDesc, initMem, initPitch);
        float regMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - tRegStart).count();
        if (regMs >= 20.0f) {
            LOGI("[PerfTrace] CreateTexture2D %ux%u fmt=%u register+decode mat %.2fms",
                 desc->width, desc->height, desc->format, regMs);
        }

        const bool isBc7Fmt = (desc->format == sgre::kDxgiBc7Unorm || desc->format == sgre::kDxgiBc7UnormSrgb);
        const uint64_t totalPixels = static_cast<uint64_t>(desc->width) * desc->height;
        if (registered && initMem != nullptr &&
            (desc->bindFlags & kD3d11BindRenderTarget) == 0 &&
            desc->usage != kD3d11UsageStaging &&
            (isBc7Fmt || totalPixels >= kPreuploadMinPixels)) {
            {
                std::lock_guard<std::mutex> lock(sPreuploadMutex);
                sPreuploadQueue.push_back(TextureUploadRequest{
                    mockTex, texDesc.generationId, 1, desc->width, desc->height
                });
            }
            sPreuploadCond.notify_one();
        }
    }
    if (ppTexture2D) *ppTexture2D = mockTex;
    return 0;
}

struct MockViewCOM : public SGRE_COM_Mock {
    void* pResource = nullptr;
};

using MockRenderTargetViewCOM = MockViewCOM;

// ID3D11View::GetResource (vtable slot 7)
static void WINAPI mock_View_GetResource(void* thisPtr, void** ppResource) {
    if (!ppResource) return;
    const auto* view = reinterpret_cast<const MockViewCOM*>(thisPtr);
    if (view != nullptr && view->pResource != nullptr) {
        *ppResource = view->pResource;
        mock_COM_AddRef(view->pResource);
    } else {
        *ppResource = nullptr;
    }
}

static int64_t WINAPI mock_Device_CreateShaderResourceView(void*, void* pResource, const void* pDesc, void** ppView) {
    static uint32_t sSrvLogCount = 0;
    if (++sSrvLogCount <= 30 || sSrvLogCount % 60 == 1) {
        sgre::TextureDesc tDesc{};
        if (sRegistry.getTextureDesc(pResource, tDesc)) {
            LOGI("CreateShaderResourceView #%u: Resource=%p (Texture %ux%u, Format=%u)",
                 sSrvLogCount, pResource, tDesc.width, tDesc.height, tDesc.format);
        } else {
            LOGI("CreateShaderResourceView #%u: Resource=%p (non-texture / buffer)",
                 sSrvLogCount, pResource);
        }
    }
    if (ppView) {
        auto* view = new MockViewCOM();
        view->vtable = gViewVtable;
        view->pResource = pResource;
        *ppView = view;
    }
    return 0;
}

static const void* sCurrentRenderTargetResource = nullptr;
static std::unordered_map<const void*, uint8_t> sOffscreenClearVals;

static int64_t WINAPI mock_Device_CreateRenderTargetView(void*, void* pResource, const void* pDesc, void** ppView) {
    if (ppView) {
        auto* view = new MockRenderTargetViewCOM();
        view->vtable = gViewVtable;
        view->pResource = pResource;
        *ppView = view;
        sgre::TextureDesc tDesc{};
        if (pResource && sRegistry.getTextureDesc(pResource, tDesc)) {
            LOGI("CreateRenderTargetView: Resource=%p (%ux%u, format=%u)", pResource, tDesc.width, tDesc.height, tDesc.format);
        } else {
            LOGI("CreateRenderTargetView: Resource=%p (Backbuffer)", pResource);
        }
    }
    return 0;
}

static int64_t WINAPI mock_Device_CreateDepthStencilView(void*, void* pResource, const void*, void** ppView) {
    if (ppView) {
        auto* view = new MockViewCOM();
        view->vtable = gViewVtable;
        view->pResource = pResource;
        *ppView = view;
        sgre::TextureDesc tDesc{};
        if (pResource && sRegistry.getTextureDesc(pResource, tDesc)) {
            LOGI("CreateDepthStencilView: Resource=%p (%ux%u, format=%u)", pResource, tDesc.width, tDesc.height, tDesc.format);
        } else {
            LOGI("CreateDepthStencilView: Resource=%p", pResource);
        }
    }
    return 0;
}

static void dumpShaderBytecode(const char* prefix, uint32_t index, const void* pBytecode, size_t bytecodeLength) {
    if (!pBytecode || bytecodeLength == 0) return;

    // Chỉ ghi bytecode ra đĩa khi biến môi trường SGRB_SHADER_DUMP_DIR được cấu hình chủ động
    // để tránh 8.356 lời gọi fopen() FUSE thất bại khi khởi động trên thiết bị thực tế.
    const char* customDir = getenv("SGRB_SHADER_DUMP_DIR");
    if (!customDir || customDir[0] == '\0') {
        return;
    }

    char filePath[512];
    snprintf(filePath, sizeof(filePath), "%s/%s_%04u.bin", customDir, prefix, index);
    FILE* fp = fopen(filePath, "wb");
    if (fp) {
        fwrite(pBytecode, 1, bytecodeLength, fp);
        fclose(fp);
        LOGI("Dumped %s shader #%u bytecode to %s (%zu bytes)", prefix, index, filePath, bytecodeLength);
    }
}

static int64_t WINAPI mock_Device_CreateVertexShader(void*, const void* pBytecode, size_t bytecodeLength, void*, void** ppShader) {
    uint32_t idx = sTotalVertexShadersCreated++;
    sTotalShadersCreated++;
    LOGI("ID3D11Device::CreateVertexShader #%u: BytecodeLength=%zu, Bytecode=%p", idx, bytecodeLength, pBytecode);
    dumpShaderBytecode("vs", idx, pBytecode, bytecodeLength);
    if (ppShader) {
        if (idx < 16) {
            sVertexShaders[idx].vtable = gGenericResourceVtable;
            sVertexShaders[idx].id = idx;
            sVertexShaders[idx].isPixelShader = false;
            *ppShader = &sVertexShaders[idx];
        } else {
            *ppShader = &gMockGenericResource;
        }
    }
    return 0;
}

static int64_t WINAPI mock_Device_CreatePixelShader(void*, const void* pBytecode, size_t bytecodeLength, void*, void** ppShader) {
    uint32_t idx = sTotalPixelShadersCreated++;
    sTotalShadersCreated++;
    if (idx < 8 || idx == 2076 || idx % 500 == 0) {
        LOGI("ID3D11Device::CreatePixelShader #%u: BytecodeLength=%zu, Bytecode=%p", idx, bytecodeLength, pBytecode);
    }
    dumpShaderBytecode("ps", idx, pBytecode, bytecodeLength);
    if (ppShader) {
        if (idx < 2080) {
            sPixelShaders[idx].vtable = gGenericResourceVtable;
            sPixelShaders[idx].id = idx;
            sPixelShaders[idx].isPixelShader = true;
            *ppShader = &sPixelShaders[idx];
        } else {
            *ppShader = &gMockGenericResource;
        }
    }
    return 0;
}

static int64_t WINAPI mock_Device_CreateShader(void*, const void* pBytecode, size_t bytecodeLength, void*, void** ppShader) {
    uint32_t idx = sTotalShadersCreated++;
    LOGI("ID3D11Device::CreateShader #%u: BytecodeLength=%zu, Bytecode=%p", idx, bytecodeLength, pBytecode);
    dumpShaderBytecode("shader", idx, pBytecode, bytecodeLength);
    if (ppShader) *ppShader = &gMockGenericResource;
    return 0;
}

struct MockInputElementDesc {
    const char* semanticName;
    uint32_t semanticIndex;
    uint32_t format;
    uint32_t inputSlot;
    uint32_t alignedByteOffset;
    uint32_t inputSlotClass;
    uint32_t instanceDataStepRate;
};

struct MockInputLayoutElement {
    std::string semanticName;
    uint32_t semanticIndex;
    uint32_t format;
    uint32_t inputSlot;
    uint32_t alignedByteOffset;
};

struct MockInputLayoutCOM : public SGRE_COM_Mock {
    std::vector<MockInputLayoutElement> elements;
    uint32_t stride = 0;

    int32_t posOffset = -1;
    uint32_t posFormat = 0;

    int32_t colorOffset = -1;
    uint32_t colorFormat = 0;

    int32_t tex0Offset = -1;
    uint32_t tex0Format = 0;

    int32_t tex1Offset = -1;
    uint32_t tex1Format = 0;
};

static uint32_t getDxgiFormatByteSize(uint32_t format) {
    switch (format) {
        case 2:  // DXGI_FORMAT_R32G32B32A32_FLOAT
        case 3:  // DXGI_FORMAT_R32G32B32A32_UINT
        case 4:  // DXGI_FORMAT_R32G32B32A32_SINT
            return 16;
        case 6:  // DXGI_FORMAT_R32G32B32_FLOAT
        case 7:  // DXGI_FORMAT_R32G32B32_UINT
        case 8:  // DXGI_FORMAT_R32G32B32_SINT
            return 12;
        case 16: // DXGI_FORMAT_R32G32_FLOAT
        case 17: // DXGI_FORMAT_R32G32_UINT
        case 18: // DXGI_FORMAT_R32G32_SINT
            return 8;
        case 28: // DXGI_FORMAT_R8G8B8A8_UNORM
        case 29: // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        case 30: // DXGI_FORMAT_R8G8B8A8_UINT
        case 31: // DXGI_FORMAT_R8G8B8A8_SNORM
        case 32: // DXGI_FORMAT_R8G8B8A8_SINT
        case 35: // DXGI_FORMAT_R16G16_UNORM
        case 41: // DXGI_FORMAT_R32_FLOAT
        case 87: // DXGI_FORMAT_B8G8R8A8_UNORM
        case 88: // DXGI_FORMAT_B8G8R8X8_UNORM
        case 91: // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            return 4;
        default:
            return 4;
    }
}

static MockInputLayoutCOM* sCurrentInputLayout = nullptr;

static int64_t WINAPI mock_Device_CreateInputLayout(void*, const void* pInputElementDescs, uint32_t numElements, const void*, size_t, void** ppLayout) {
    auto* layout = new MockInputLayoutCOM();
    layout->vtable = gGenericResourceVtable;
    if (pInputElementDescs && numElements > 0) {
        const auto* descs = reinterpret_cast<const MockInputElementDesc*>(pInputElementDescs);
        uint32_t currentOffset = 0;
        for (uint32_t i = 0; i < numElements; ++i) {
            MockInputLayoutElement elem;
            elem.semanticName = descs[i].semanticName ? descs[i].semanticName : "";
            elem.semanticIndex = descs[i].semanticIndex;
            elem.format = descs[i].format;
            elem.inputSlot = descs[i].inputSlot;
            uint32_t elemSize = getDxgiFormatByteSize(elem.format);
            if (descs[i].alignedByteOffset == 0xFFFFFFFF) {
                elem.alignedByteOffset = currentOffset;
            } else {
                elem.alignedByteOffset = descs[i].alignedByteOffset;
            }
            currentOffset = elem.alignedByteOffset + elemSize;

            if (elem.semanticName == "POSITION" && elem.semanticIndex == 0) {
                layout->posOffset = static_cast<int32_t>(elem.alignedByteOffset);
                layout->posFormat = elem.format;
            } else if (elem.semanticName == "COLOR" && elem.semanticIndex == 0) {
                layout->colorOffset = static_cast<int32_t>(elem.alignedByteOffset);
                layout->colorFormat = elem.format;
            } else if (elem.semanticName == "TEXCOORD") {
                if (elem.semanticIndex == 0) {
                    layout->tex0Offset = static_cast<int32_t>(elem.alignedByteOffset);
                    layout->tex0Format = elem.format;
                } else if (elem.semanticIndex == 1) {
                    layout->tex1Offset = static_cast<int32_t>(elem.alignedByteOffset);
                    layout->tex1Format = elem.format;
                }
            }
            layout->elements.push_back(elem);
            LOGI("CreateInputLayout: elem[%u] Name=%s, Idx=%u, Fmt=%u, Slot=%u, Offset=%u (size=%u)",
                 i, elem.semanticName.c_str(), elem.semanticIndex, elem.format, elem.inputSlot, elem.alignedByteOffset, elemSize);
        }
        layout->stride = currentOffset;
    }
    if (ppLayout) *ppLayout = layout;
    return 0;
}

static int64_t WINAPI mock_Device_CreateBlendState(void*, const void* pDesc, void** ppState) {
    auto* mockState = new MockBlendStateCOM();
    mockState->vtable = gGenericResourceVtable;
    if (pDesc) {
        mockState->desc = *reinterpret_cast<const MockBlendDesc*>(pDesc);
        static uint32_t sBsCount = 0;
        const auto& rt0 = mockState->desc.renderTarget[0];
        if (++sBsCount <= 5) {
            LOGI("CreateBlendState #%u (%p): RT0 Blend=%d, SrcRGB=%u, DstRGB=%u, OpRGB=%u, SrcA=%u, DstA=%u, OpA=%u, Mask=0x%02X",
                 sBsCount, mockState, rt0.blendEnable, rt0.srcBlend, rt0.destBlend, rt0.blendOp,
                 rt0.srcBlendAlpha, rt0.destBlendAlpha, rt0.blendOpAlpha, rt0.renderTargetWriteMask);
        }
    } else {
        memset(&mockState->desc, 0, sizeof(mockState->desc));
        mockState->desc.renderTarget[0].renderTargetWriteMask = 0x0F;
    }
    if (ppState) *ppState = mockState;
    return 0;
}

static int64_t WINAPI mock_Device_CreateDepthStencilState(void*, const void* pDesc, void** ppState) {
    auto* mockState = new MockDepthStencilStateCOM();
    mockState->vtable = gGenericResourceVtable;
    if (pDesc) {
        mockState->desc = *reinterpret_cast<const MockDepthStencilDesc*>(pDesc);
        static uint32_t sDssDump = 0;
        if (++sDssDump <= 5) {
            LOGI("CreateDepthStencilState #%u: StencilEnable=%d, FrontFunc=%u, PassOp=%u, WriteMask=0x%02X",
                 sDssDump, mockState->desc.stencilEnable, mockState->desc.frontFace.stencilFunc,
                 mockState->desc.frontFace.stencilPassOp, mockState->desc.stencilWriteMask);
        }
    } else {
        memset(&mockState->desc, 0, sizeof(mockState->desc));
    }
    if (ppState) *ppState = mockState;
    return 0;
}

static int64_t WINAPI mock_Device_CreateRasterizerState(void*, const void* pDesc, void** ppState) {
    auto* mockState = new MockRasterizerStateCOM();
    mockState->vtable = gGenericResourceVtable;
    if (pDesc) {
        mockState->desc = *reinterpret_cast<const MockRasterizerDesc*>(pDesc);
        static uint32_t sRsCount = 0;
        if (++sRsCount <= 5) {
            LOGI("CreateRasterizerState #%u (%p): ScissorEnable=%d, CullMode=%d",
                 sRsCount, mockState, mockState->desc.scissorEnable, mockState->desc.cullMode);
        }
    } else {
        memset(&mockState->desc, 0, sizeof(mockState->desc));
    }
    if (ppState) *ppState = mockState;
    return 0;
}

static int64_t WINAPI mock_Device_CreateState(void*, const void*, void** ppState) {
    if (ppState) *ppState = &gMockGenericResource;
    return 0;
}

static uint32_t WINAPI mock_Device_GetFeatureLevel(void*) {
    return 0xb000; // D3D_FEATURE_LEVEL_11_0
}

static int64_t WINAPI mock_Device_GetDeviceRemovedReason(void*) {
    return 0; // S_OK
}

static void WINAPI mock_Device_GetImmediateContext(void*, void** ppImmediateContext) {
    if (ppImmediateContext) *ppImmediateContext = &gMockContext;
}

static void emitMeshDrawCommand(uint32_t count, uint32_t startLocation, int32_t baseVertexLocation, bool isIndexed) {
    if (sCurrentVertexBuffer == nullptr || count < 3) {
        return;
    }

    sgre::MappedView vView{};
    sgre::BufferDesc vDesc{};
    if (!sRegistry.getBufferDesc(sCurrentVertexBuffer, vDesc) || !sRegistry.map(sCurrentVertexBuffer, vView) || vView.data == nullptr) {
        return;
    }

    sgre::MappedView iView{};
    const uint16_t* idx16 = nullptr;
    const uint32_t* idx32 = nullptr;
    if (isIndexed && sCurrentIndexBuffer != nullptr && sRegistry.map(sCurrentIndexBuffer, iView) && iView.data != nullptr) {
        const uint8_t* indexDataPtr = iView.data + sCurrentIndexOffset;
        if (sCurrentIndexFormat == 57) { // DXGI_FORMAT_R16_UINT
            idx16 = reinterpret_cast<const uint16_t*>(indexDataPtr);
        } else if (sCurrentIndexFormat == 42) { // DXGI_FORMAT_R32_UINT
            idx32 = reinterpret_cast<const uint32_t*>(indexDataPtr);
        }
    }

    MeshDrawCommand meshCmd{};
    meshCmd.vsShaderId = sCurrentVertexShaderId;
    memset(meshCmd.vsCb0, 0, sizeof(meshCmd.vsCb0));

    // Fallback Identity Matrix và màu trắng mặc định nếu chưa nạp Constant Buffer
    meshCmd.vsCb0[0]  = 1.0f;
    meshCmd.vsCb0[5]  = 1.0f;
    meshCmd.vsCb0[10] = 1.0f;
    meshCmd.vsCb0[15] = 1.0f;
    meshCmd.vsCb0[16] = 1.0f;
    meshCmd.vsCb0[17] = 1.0f;
    meshCmd.vsCb0[18] = 1.0f;
    meshCmd.vsCb0[19] = 1.0f;

    if (sBoundConstantBuffersVS[0] != nullptr) {
        sgre::MappedView vsView{};
        sgre::BufferDesc vsDesc{};
        if (sRegistry.map(sBoundConstantBuffersVS[0], vsView) && vsView.data != nullptr) {
            size_t copyBytes = sizeof(meshCmd.vsCb0);
            if (sRegistry.getBufferDesc(sBoundConstantBuffersVS[0], vsDesc) && vsDesc.byteWidth > 0) {
                copyBytes = std::min(copyBytes, static_cast<size_t>(vsDesc.byteWidth));
            }
            memcpy(meshCmd.vsCb0, vsView.data, copyBytes);
            static uint32_t sVsMatLog = 0;
            if (++sVsMatLog <= 5) {
                LOGI("VS_CB0 #%u (vsId=%u): R0=[%.4f, %.4f, %.4f, %.4f] R1=[%.4f, %.4f, %.4f, %.4f] R2=[%.4f, %.4f, %.4f, %.4f] R3=[%.4f, %.4f, %.4f, %.4f]",
                     sVsMatLog, meshCmd.vsShaderId,
                     meshCmd.vsCb0[0], meshCmd.vsCb0[1], meshCmd.vsCb0[2], meshCmd.vsCb0[3],
                     meshCmd.vsCb0[4], meshCmd.vsCb0[5], meshCmd.vsCb0[6], meshCmd.vsCb0[7],
                     meshCmd.vsCb0[8], meshCmd.vsCb0[9], meshCmd.vsCb0[10], meshCmd.vsCb0[11],
                     meshCmd.vsCb0[12], meshCmd.vsCb0[13], meshCmd.vsCb0[14], meshCmd.vsCb0[15]);
            }
        }
    }

    meshCmd.shaderId = sCurrentPixelShaderId;
    memset(meshCmd.cb0, 0, sizeof(meshCmd.cb0));
    if (sBoundConstantBuffersPS[0] != nullptr) {
        sgre::MappedView cbView{};
        sgre::BufferDesc psDesc{};
        if (sRegistry.map(sBoundConstantBuffersPS[0], cbView) && cbView.data != nullptr) {
            size_t copyBytes = sizeof(meshCmd.cb0);
            if (sRegistry.getBufferDesc(sBoundConstantBuffersPS[0], psDesc) && psDesc.byteWidth > 0) {
                copyBytes = std::min(copyBytes, static_cast<size_t>(psDesc.byteWidth));
            }
            memcpy(meshCmd.cb0, cbView.data, copyBytes);
        }
    }

    const void* colorTex = sBoundTextureResources[0];
    meshCmd.textureHandle = colorTex;
    meshCmd.textureWidth = 0;
    meshCmd.textureHeight = 0;
    if (colorTex != nullptr) {
        sgre::TextureDesc tDesc{};
        if (sRegistry.getTextureDesc(colorTex, tDesc)) {
            meshCmd.textureWidth = tDesc.width;
            meshCmd.textureHeight = tDesc.height;
        }
    }

    meshCmd.alphaTextureHandle = nullptr;
    meshCmd.alphaTextureWidth = 0;
    meshCmd.alphaTextureHeight = 0;
    if (isShaderUsingT1(meshCmd.shaderId)) {
        const void* alphaTex = sBoundTextureResources[1];
        if (alphaTex != nullptr) {
            sgre::TextureDesc alphaDesc{};
            if (sRegistry.getTextureDesc(alphaTex, alphaDesc)) {
                meshCmd.alphaTextureHandle = alphaTex;
                meshCmd.alphaTextureWidth = alphaDesc.width;
                meshCmd.alphaTextureHeight = alphaDesc.height;
            }
        }
    }

    meshCmd.destTextureHandle = nullptr;
    meshCmd.destTextureWidth = 0;
    meshCmd.destTextureHeight = 0;
    if (isShaderUsingT2(meshCmd.shaderId)) {
        const void* destTex = sBoundTextureResources[2];
        if (destTex != nullptr) {
            sgre::TextureDesc destDesc{};
            if (sRegistry.getTextureDesc(destTex, destDesc)) {
                meshCmd.destTextureHandle = destTex;
                meshCmd.destTextureWidth = destDesc.width;
                meshCmd.destTextureHeight = destDesc.height;
            }
        }
    }

    meshCmd.stencilEnable = (sCurrentDepthStencilState != nullptr && sCurrentDepthStencilState->desc.stencilEnable) ? 1 : 0;
    meshCmd.stencilRef = static_cast<uint8_t>(sCurrentStencilRef & 0xFF);
    if (sCurrentDepthStencilState != nullptr && meshCmd.stencilEnable) {
        meshCmd.stencilReadMask = sCurrentDepthStencilState->desc.stencilReadMask;
        meshCmd.stencilWriteMask = sCurrentDepthStencilState->desc.stencilWriteMask;
        meshCmd.stencilFunc = d3dComparisonToGl(sCurrentDepthStencilState->desc.frontFace.stencilFunc);
        meshCmd.stencilFailOp = d3dStencilOpToGl(sCurrentDepthStencilState->desc.frontFace.stencilFailOp);
        meshCmd.stencilDepthFailOp = d3dStencilOpToGl(sCurrentDepthStencilState->desc.frontFace.stencilDepthFailOp);
        meshCmd.stencilPassOp = d3dStencilOpToGl(sCurrentDepthStencilState->desc.frontFace.stencilPassOp);
    } else {
        meshCmd.stencilReadMask = 0xFF;
        meshCmd.stencilWriteMask = 0xFF;
        meshCmd.stencilFunc = 0x0207;
        meshCmd.stencilFailOp = 0x1E00;
        meshCmd.stencilDepthFailOp = 0x1E00;
        meshCmd.stencilPassOp = 0x1E00;
    }
    meshCmd.colorWriteMask = (sCurrentBlendState != nullptr) ? sCurrentBlendState->desc.renderTarget[0].renderTargetWriteMask : 0x0F;

    if (sCurrentBlendState != nullptr) {
        const auto& rt0 = sCurrentBlendState->desc.renderTarget[0];
        meshCmd.blendEnable = rt0.blendEnable ? 1 : 0;
        meshCmd.srcBlend = d3dBlendToGl(rt0.srcBlend);
        meshCmd.destBlend = d3dBlendToGl(rt0.destBlend);
        meshCmd.blendOp = d3dBlendOpToGl(rt0.blendOp);
        meshCmd.srcBlendAlpha = d3dBlendToGl(rt0.srcBlendAlpha);
        meshCmd.destBlendAlpha = d3dBlendToGl(rt0.destBlendAlpha);
        meshCmd.blendOpAlpha = d3dBlendOpToGl(rt0.blendOpAlpha);
    } else {
        meshCmd.blendEnable = 0;
        meshCmd.srcBlend = 0x0302; // GL_SRC_ALPHA
        meshCmd.destBlend = 0x0303; // GL_ONE_MINUS_SRC_ALPHA
        meshCmd.blendOp = 0x8006;   // GL_FUNC_ADD
        meshCmd.srcBlendAlpha = 0x0302;
        meshCmd.destBlendAlpha = 0x0303;
        meshCmd.blendOpAlpha = 0x8006;
    }
    memcpy(meshCmd.blendFactor, sCurrentBlendFactor, sizeof(meshCmd.blendFactor));

    meshCmd.viewportX = sCurrentViewport.topLeftX;
    meshCmd.viewportY = sCurrentViewport.topLeftY;
    meshCmd.viewportW = sCurrentViewport.width > 0.0f ? sCurrentViewport.width : static_cast<float>(gCurrentWidth);
    meshCmd.viewportH = sCurrentViewport.height > 0.0f ? sCurrentViewport.height : static_cast<float>(gCurrentHeight);

    if (sCurrentRasterizerState != nullptr && sCurrentRasterizerState->desc.scissorEnable && sHasScissorRect) {
        int32_t scL = sCurrentScissorRect.left;
        int32_t scT = sCurrentScissorRect.top;
        int32_t scR = sCurrentScissorRect.right;
        int32_t scB = sCurrentScissorRect.bottom;
        meshCmd.scissorEnable = 1;
        meshCmd.scissorX = scL;
        meshCmd.scissorY = scT;
        meshCmd.scissorW = std::max(0, scR - scL);
        meshCmd.scissorH = std::max(0, scB - scT);
    } else {
        meshCmd.scissorEnable = 0;
        meshCmd.scissorX = 0;
        meshCmd.scissorY = 0;
        meshCmd.scissorW = static_cast<int32_t>(gCurrentWidth);
        meshCmd.scissorH = static_cast<int32_t>(gCurrentHeight);
    }

    meshCmd.isClearCommand = 0;
    meshCmd.clearStencil = 0;
    bool isOffscreen = (sCurrentRenderTargetResource != nullptr && sCurrentRenderTargetResource != &gMockTexture2D);
    meshCmd.renderTargetHandle = isOffscreen ? sCurrentRenderTargetResource : nullptr;
    meshCmd.renderTargetWidth = 1920;
    meshCmd.renderTargetHeight = 1080;
    if (isOffscreen) {
        sgre::TextureDesc rtDesc{};
        if (sRegistry.getTextureDesc(sCurrentRenderTargetResource, rtDesc)) {
            meshCmd.renderTargetWidth = rtDesc.width;
            meshCmd.renderTargetHeight = rtDesc.height;
        }
    }

    std::unordered_map<int32_t, uint16_t> vertexRemap;
    meshCmd.indexData.reserve(count * 3);

    auto getRawIndex = [&](uint32_t k) -> int32_t {
        if (isIndexed) {
            int32_t vIdx = static_cast<int32_t>(k);
            if (idx16 != nullptr) {
                vIdx = static_cast<int32_t>(idx16[startLocation + k]);
            } else if (idx32 != nullptr) {
                vIdx = static_cast<int32_t>(idx32[startLocation + k]);
            }
            return vIdx + baseVertexLocation;
        } else {
            return static_cast<int32_t>(startLocation + k);
        }
    };

    auto getOrAddVertex = [&](int32_t vertexIdx) -> int32_t {
        if (vertexIdx < 0) return -1;
        auto it = vertexRemap.find(vertexIdx);
        if (it != vertexRemap.end()) {
            return it->second;
        }
        size_t byteOffset = sCurrentVertexOffset + static_cast<size_t>(vertexIdx) * sCurrentVertexStride;
        if (byteOffset + sCurrentVertexStride > vDesc.byteWidth) return -1;

        const uint8_t* vertPtr = vView.data + byteOffset;

        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float u0 = 0.0f, v0 = 0.0f;
        float u1 = 0.0f, v1 = 0.0f;
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

        if (sCurrentInputLayout != nullptr) {
            int32_t effPosOffset = sCurrentInputLayout->posOffset;
            int32_t effTex0Offset = sCurrentInputLayout->tex0Offset;
            int32_t effTex1Offset = sCurrentInputLayout->tex1Offset;
            int32_t effColorOffset = sCurrentInputLayout->colorOffset;

            // 1. POSITION
            if (effPosOffset >= 0 && byteOffset + effPosOffset + 8 <= vDesc.byteWidth) {
                const float* pPos = reinterpret_cast<const float*>(vertPtr + effPosOffset);
                posX = pPos[0];
                posY = pPos[1];
                if (sCurrentInputLayout->posFormat == 6 && byteOffset + effPosOffset + 12 <= vDesc.byteWidth) {
                    float z = pPos[2];
                    posZ = (std::isnan(z) || std::isinf(z)) ? 0.0f : z;
                }
            }

            // 2. TEXCOORD0
            if (effTex0Offset >= 0 && byteOffset + effTex0Offset + 8 <= vDesc.byteWidth) {
                const float* pUv0 = reinterpret_cast<const float*>(vertPtr + effTex0Offset);
                u0 = pUv0[0];
                v0 = pUv0[1];
                if (std::isnan(u0) || std::isinf(u0)) u0 = 0.0f;
                if (std::isnan(v0) || std::isinf(v0)) v0 = 0.0f;
            }

            // 3. TEXCOORD1
            if (effTex1Offset >= 0 && byteOffset + effTex1Offset + 8 <= vDesc.byteWidth) {
                const float* pUv1 = reinterpret_cast<const float*>(vertPtr + effTex1Offset);
                u1 = pUv1[0];
                v1 = pUv1[1];
                if (std::isnan(u1) || std::isinf(u1)) u1 = 0.0f;
                if (std::isnan(v1) || std::isinf(v1)) v1 = 0.0f;
            }

            // 4. COLOR
            if (effColorOffset >= 0 && byteOffset + effColorOffset + 4 <= vDesc.byteWidth) {
                const uint8_t* cPtr = vertPtr + effColorOffset;
                if (sCurrentInputLayout->colorFormat == 87 || sCurrentInputLayout->colorFormat == 91) { // B8G8R8A8_UNORM
                    b = static_cast<float>(cPtr[0]) / 255.0f;
                    g = static_cast<float>(cPtr[1]) / 255.0f;
                    r = static_cast<float>(cPtr[2]) / 255.0f;
                    a = static_cast<float>(cPtr[3]) / 255.0f;
                } else if (sCurrentInputLayout->colorFormat == 28 || sCurrentInputLayout->colorFormat == 29) { // R8G8B8A8_UNORM
                    r = static_cast<float>(cPtr[0]) / 255.0f;
                    g = static_cast<float>(cPtr[1]) / 255.0f;
                    b = static_cast<float>(cPtr[2]) / 255.0f;
                    a = static_cast<float>(cPtr[3]) / 255.0f;
                } else if (sCurrentInputLayout->colorFormat == 2 && byteOffset + effColorOffset + 16 <= vDesc.byteWidth) { // R32G32B32A32_FLOAT
                    const float* fCol = reinterpret_cast<const float*>(cPtr);
                    r = fCol[0]; g = fCol[1]; b = fCol[2]; a = fCol[3];
                }
            } else if (sCurrentInputLayout->colorOffset < 0) {
                // Mesh không có COLOR trong input layout, nhận màu từ Constant Buffer cb0[4] của Vertex Shader
                r = meshCmd.vsCb0[16];
                g = meshCmd.vsCb0[17];
                b = meshCmd.vsCb0[18];
                a = meshCmd.vsCb0[19];
            }
        } else {
            // Fallback an toàn nếu InputLayout chưa set
            const float* fPtr = reinterpret_cast<const float*>(vertPtr);
            posX = fPtr[0];
            posY = fPtr[1];
            if (sCurrentVertexStride >= 20) {
                const float* pUv = reinterpret_cast<const float*>(vertPtr + 12);
                u0 = pUv[0];
                v0 = pUv[1];
            }
        }

        if (std::isnan(posX) || std::isinf(posX)) posX = 0.0f;
        if (std::isnan(posY) || std::isinf(posY)) posY = 0.0f;
        if (std::isnan(posZ) || std::isinf(posZ)) posZ = 0.0f;
        if (std::isnan(u0) || std::isinf(u0)) u0 = 0.0f;
        if (std::isnan(v0) || std::isinf(v0)) v0 = 0.0f;
        if (std::isnan(u1) || std::isinf(u1)) u1 = 0.0f;
        if (std::isnan(v1) || std::isinf(v1)) v1 = 0.0f;

        uint16_t newIdx = static_cast<uint16_t>(meshCmd.vertexData.size() / 11);
        meshCmd.vertexData.push_back(posX);
        meshCmd.vertexData.push_back(posY);
        meshCmd.vertexData.push_back(posZ);
        meshCmd.vertexData.push_back(u0);
        meshCmd.vertexData.push_back(v0);
        meshCmd.vertexData.push_back(u1);
        meshCmd.vertexData.push_back(v1);
        meshCmd.vertexData.push_back(r);
        meshCmd.vertexData.push_back(g);
        meshCmd.vertexData.push_back(b);
        meshCmd.vertexData.push_back(a);

        vertexRemap[vertexIdx] = newIdx;
        return static_cast<int32_t>(newIdx);
    };

    // Giải mã topology D3D11 sang Triangle List (GL_TRIANGLES)
    bool isStrip = (sCurrentPrimitiveTopology == 5) || (sCurrentPrimitiveTopology != 4 && count >= 4 && count % 6 != 0);
    if (isStrip) {
        bool flipWinding = false;
        for (uint32_t k = 0; k + 2 < count; ++k) {
            int32_t rawA = getRawIndex(k);
            int32_t rawB = getRawIndex(k + 1);
            int32_t rawC = getRawIndex(k + 2);

            // Bỏ qua strip cut index (0xFFFF / 0xFFFFFFFF) hoặc chỉ số không hợp lệ
            if (rawA == 0xFFFF || rawB == 0xFFFF || rawC == 0xFFFF ||
                rawA == -1 || rawB == -1 || rawC == -1) {
                flipWinding = false;
                continue;
            }

            // Bỏ qua các tam giác suy biến (degenerate triangles để tách quad/strip)
            if (rawA == rawB || rawB == rawC || rawA == rawC) {
                flipWinding = false;
                continue;
            }

            // Giữ nguyên winding CCW cho tam giác lẻ trong triangle strip
            if (flipWinding) {
                std::swap(rawB, rawC);
            }
            flipWinding = !flipWinding;

            int32_t idxA = getOrAddVertex(rawA);
            int32_t idxB = getOrAddVertex(rawB);
            int32_t idxC = getOrAddVertex(rawC);

            if (idxA >= 0 && idxB >= 0 && idxC >= 0) {
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxA));
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxB));
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxC));
            }
        }
    } else {
        for (uint32_t k = 0; k + 2 < count; k += 3) {
            int32_t rawA = getRawIndex(k);
            int32_t rawB = getRawIndex(k + 1);
            int32_t rawC = getRawIndex(k + 2);

            if (rawA == rawB || rawB == rawC || rawA == rawC) {
                continue;
            }

            int32_t idxA = getOrAddVertex(rawA);
            int32_t idxB = getOrAddVertex(rawB);
            int32_t idxC = getOrAddVertex(rawC);

            if (idxA >= 0 && idxB >= 0 && idxC >= 0) {
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxA));
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxB));
                meshCmd.indexData.push_back(static_cast<uint16_t>(idxC));
            }
        }
    }

    if (meshCmd.indexData.size() >= 3) {
        std::lock_guard<std::mutex> lock(sSpriteMutex);
        sActiveDrawingMeshes.push_back(std::move(meshCmd));
    }
}

// ID3D11DeviceContext methods
static void WINAPI mock_Context_DrawIndexed(void*, uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) {
    sTotalDrawIndexedCalls++;
    static uint32_t sDrawIndexedCount = 0;
    bool shouldLog = (++sDrawIndexedCount % 6000 == 1);
    if (shouldLog) {
        LOGI("DrawIndexed #%u: Count=%u, Topo=%u, Start=%u, Base=%d",
             sDrawIndexedCount, indexCount, sCurrentPrimitiveTopology, startIndexLocation, baseVertexLocation);
    }
    emitMeshDrawCommand(indexCount, startIndexLocation, baseVertexLocation, true);
}

static void WINAPI mock_Context_Draw(void*, uint32_t vertexCount, uint32_t startVertexLocation) {
    sTotalDrawCalls++;
    static uint32_t sDrawCount = 0;
    bool shouldLog = (++sDrawCount <= 5);
    if (shouldLog) {
        LOGI("ID3D11DeviceContext::Draw #%u: VertexCount=%u, StartVertex=%u, Topo=%u, Vb=%p",
             sDrawCount, vertexCount, startVertexLocation, sCurrentPrimitiveTopology, sCurrentVertexBuffer);
    }
    emitMeshDrawCommand(vertexCount, startVertexLocation, 0, false);
}

inline bool isCaptureOrThumbnailSize(uint32_t w, uint32_t h) {
    return (w == 480 && h == 270) ||
           (w == 568 && h == 320) ||
           (w == 284 && h == 160);
}

static inline bool isReadMapType(uint32_t mapType) {
    return mapType == kD3d11MapRead || mapType == kD3d11MapReadWrite;
}

static inline bool isBgraDxgiFormat(uint32_t format) {
    return format == sgre::kDxgiB8G8R8A8Unorm || format == sgre::kDxgiB8G8R8X8Unorm;
}

static bool isGpuBackedResource(const void* resource, sgre::TextureDesc& outDesc, bool& outIsRenderTarget) {
    outIsRenderTarget = false;
    if (resource == nullptr) return false;
    if (resource == &gMockTexture2D || resource == sCurrentRenderTargetResource) {
        outIsRenderTarget = true;
        sRegistry.getTextureDesc(resource, outDesc);
        return true;
    }
    if (sRegistry.getTextureDesc(resource, outDesc)) {
        outIsRenderTarget = (outDesc.bindFlags & kD3d11BindRenderTarget) != 0;
        return outIsRenderTarget || sRegistry.isGpuOwned(resource);
    }
    return false;
}

static bool requestGpuFlushAndReadback(
    const void* resource, uint8_t* dstPtr, const sgre::TextureDesc& desc, uint32_t rowPitch
) {
    if (!resource || !dstPtr || desc.width == 0 || desc.height == 0 || !sRenderLoopActive.load()) {
        return false;
    }

    // Chờ frame trước hoàn tất thực thi trên Render Thread để dữ liệu GPU đồng nhất
    {
        std::unique_lock<std::mutex> fLock(sFrameMutex);
        if (sNewFrameAvailable || sFramePendingAck.load() || sSpritePendingConsume.load()) {
            sFrameCond.notify_all();
            sFrameAcknowledgeCond.wait_for(fLock, std::chrono::milliseconds(kGpuReadbackTimeoutMs), [] {
                return (!sNewFrameAvailable && !sFramePendingAck.load() && !sSpritePendingConsume.load()) ||
                       !sRenderLoopActive.load();
            });
        }
    }
    if (!sRenderLoopActive.load()) {
        return false;
    }

    std::unique_lock<std::mutex> flushLock(sFlushMutex);
    sPendingFlushRequest = GpuFlushRequest{};
    sPendingFlushRequest.targetHandle = resource;
    sPendingFlushRequest.dstPtr = dstPtr;
    sPendingFlushRequest.width = desc.width;
    sPendingFlushRequest.height = desc.height;
    sPendingFlushRequest.rowPitch = rowPitch > 0 ? rowPitch : desc.width * 4;
    sPendingFlushRequest.toBgra = isBgraDxgiFormat(desc.format);
    {
        std::lock_guard<std::mutex> sLock(sSpriteMutex);
        sPendingFlushRequest.commands = std::move(sActiveDrawingMeshes);
        sActiveDrawingMeshes.clear();
    }
    sFlushPending = true;
    sFlushDone = false;
    sFlushSucceeded = false;

    D3D11Bridge_WakeRenderThread();
    sFlushCond.wait_for(flushLock, std::chrono::milliseconds(kGpuReadbackTimeoutMs), [] {
        return sFlushDone || !sRenderLoopActive.load();
    });

    const bool ok = sFlushDone && sFlushSucceeded;
    sFlushPending = false;
    sFlushDone = false;
    return ok;
}

static int64_t WINAPI mock_Context_Map(void*, void* pResource, uint32_t subresource, uint32_t mapType, uint32_t mapFlags, void* pMappedResource) {
    static uint32_t sMapCount = 0;
    if (++sMapCount <= 5) {
        LOGI("ID3D11DeviceContext::Map: Resource=%p, Subresource=%u, Type=%u, Flags=0x%x",
             pResource, subresource, mapType, mapFlags);
    }
    if (pMappedResource) {
        struct MockMappedSubresource {
            void* pData;
            uint32_t rowPitch;
            uint32_t depthPitch;
        } *mapped = reinterpret_cast<MockMappedSubresource*>(pMappedResource);

        sgre::MappedView view{};
        if (pResource && sRegistry.map(pResource, view)) {
            mapped->pData = view.data;
            mapped->rowPitch = view.rowPitch;
            mapped->depthPitch = view.depthPitch;

            sgre::TextureDesc desc{};
            bool hasTexDesc = sRegistry.getTextureDesc(pResource, desc);
            bool gpuOwned = hasTexDesc && sRegistry.isGpuOwned(pResource);
            bool readFromGpu = gpuOwned && isReadMapType(mapType);

            {
                std::lock_guard<std::mutex> mapLock(sMapTrackingMutex);
                sActiveMappedModes[pResource] = { mapType, readFromGpu };
            }

            // Flush lệnh GPU đang chờ và readback đúng dữ liệu từ VRAM của chính resource này (như v42_staging của Capture::storeThumbnail)
            if (readFromGpu) {
                if (requestGpuFlushAndReadback(pResource, view.data, desc, view.rowPitch)) {
                    LOGI("mock_Context_Map: Readback GPU thanh cong cho Staging %p (%ux%u, fmt=%u)",
                         pResource, desc.width, desc.height, desc.format);
                } else {
                    LOGW("mock_Context_Map: Readback GPU that bai hoac timeout cho %p (%ux%u)",
                         pResource, desc.width, desc.height);
                }
            }
        } else {
            mapped->pData = sMappedScratchMemory;
            mapped->rowPitch = 4096;
            mapped->depthPitch = 4096 * 1080;
        }
    }
    return 0; // S_OK
}

static void WINAPI mock_Context_Unmap(void*, void* pResource, uint32_t subresource) {
    static uint32_t sUnmapCount = 0;
    if (++sUnmapCount <= 5) {
        LOGI("ID3D11DeviceContext::Unmap: Resource=%p, Subresource=%u", pResource, subresource);
    }
    if (!pResource) return;

    ActiveMapInfo mapInfo{};
    {
        std::lock_guard<std::mutex> mapLock(sMapTrackingMutex);
        auto it = sActiveMappedModes.find(pResource);
        if (it != sActiveMappedModes.end()) {
            mapInfo = it->second;
            sActiveMappedModes.erase(it);
        }
    }

    // Nếu Unmap sau tác vụ đọc từ GPU (D3D11_MAP_READ hoặc readback Staging), không đánh dấu CPU overwrite
    if (mapInfo.readFromGpu || mapInfo.mapType == kD3d11MapRead) {
        return;
    }

    sgre::TextureDesc desc{};
    if (sRegistry.getTextureDesc(pResource, desc)) {
        // Khi Load thumbnail từ save file, hàm sub_1400802C0 của Mages Engine ghi dữ liệu BGRA vào texture slot UI.
        // Nếu texture là Thumbnail UI (284x160, 480x270, 568x320) có Format 87/88 (BGRA)
        // và không phải Staging Texture (usage != 3), ta hoán đổi B <-> R để OpenGL ES hiển thị đúng chuẩn RGBA trên UI.
        if (isCaptureOrThumbnailSize(desc.width, desc.height) && isBgraDxgiFormat(desc.format) &&
            desc.usage != kD3d11UsageStaging) {
            sgre::MappedView view{};
            if (sRegistry.map(pResource, view) && view.data) {
                uint8_t* pData = static_cast<uint8_t*>(view.data);
                for (uint32_t y = 0; y < desc.height; ++y) {
                    uint8_t* pRow = pData + static_cast<size_t>(y) * view.rowPitch;
                    for (uint32_t x = 0; x < desc.width; ++x) {
                        uint8_t* px = pRow + static_cast<size_t>(x) * 4;
                        std::swap(px[0], px[2]); // B <-> R
                    }
                }
                LOGI("mock_Context_Unmap: Swizzled Thumbnail Texture %p (%ux%u) BGRA -> RGBA",
                     pResource, desc.width, desc.height);
            }
        }
    }
    sRegistry.setGpuOwned(pResource, false);
    sRegistry.touchTexture(pResource);
}

static void WINAPI mock_Context_RSSetViewports(void*, uint32_t numViewports, const MockViewport* pViewports) {
    if (numViewports > 0 && pViewports) {
        sCurrentViewport = pViewports[0];
        static uint32_t sVpCount = 0;
        if (++sVpCount <= 5) {
            LOGI("ID3D11DeviceContext::RSSetViewports: [%.0f, %.0f, %.0fx%.0f, depth: %.1f-%.1f]",
                 pViewports[0].topLeftX, pViewports[0].topLeftY,
                 pViewports[0].width, pViewports[0].height,
                 pViewports[0].minDepth, pViewports[0].maxDepth);
        }
    }
}

static void WINAPI mock_Context_RSSetScissorRects(void*, uint32_t numRects, const MockRect* pRects) {
    if (numRects > 0 && pRects) {
        sCurrentScissorRect = pRects[0];
        sHasScissorRect = true;
        static uint32_t sScissorCount = 0;
        if (++sScissorCount <= 5) {
            LOGI("ID3D11DeviceContext::RSSetScissorRects: [%d, %d, %d, %d]",
                 pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom);
        }
    } else {
        sHasScissorRect = false;
    }
}

static void WINAPI mock_Context_UpdateSubresource(
    void*, void* pDstResource, uint32_t dstSubresource, const void* pDstBox,
    const void* pSrcData, uint32_t srcRowPitch, uint32_t srcDepthPitch
) {
    static uint32_t sUpdateCount = 0;
    if (++sUpdateCount <= 5) {
        LOGI("ID3D11DeviceContext::UpdateSubresource: Dst=%p, Subres=%u, Src=%p, RowPitch=%u, DepthPitch=%u",
             pDstResource, dstSubresource, pSrcData, srcRowPitch, srcDepthPitch);
    }
    if (pDstResource && pSrcData) {
        const auto* box = reinterpret_cast<const sgre::UpdateBox*>(pDstBox);
        if (!sRegistry.updateBuffer(pDstResource, box, pSrcData)) {
            if (sRegistry.updateTexture(pDstResource, box, pSrcData, srcRowPitch)) {
                sLastUpdatedTextureResource = pDstResource;
            }
        }
    }
}

static void enqueueGpuTextureCopy(
    void* pDstResource, const sgre::TextureDesc& dstDesc, uint32_t dstX, uint32_t dstY,
    void* pSrcResource, const sgre::TextureDesc& srcDesc, const sgre::UpdateBox* srcBox
) {
    const uint32_t sW = srcDesc.width > 0 ? srcDesc.width : gCurrentWidth;
    const uint32_t sH = srcDesc.height > 0 ? srcDesc.height : gCurrentHeight;
    const uint32_t dW = dstDesc.width > 0 ? dstDesc.width : gCurrentWidth;
    const uint32_t dH = dstDesc.height > 0 ? dstDesc.height : gCurrentHeight;

    MeshDrawCommand copyCmd{};
    copyCmd.isClearCommand = kCmdCopySubresource;
    copyCmd.renderTargetHandle = (pSrcResource != &gMockTexture2D) ? pSrcResource : nullptr;
    copyCmd.renderTargetWidth = sW;
    copyCmd.renderTargetHeight = sH;
    copyCmd.textureHandle = pDstResource;
    copyCmd.textureWidth = dW;
    copyCmd.textureHeight = dH;
    copyCmd.viewportX = static_cast<float>(dstX);
    copyCmd.viewportY = static_cast<float>(dstY);
    if (srcBox != nullptr) {
        copyCmd.scissorX = static_cast<int32_t>(srcBox->left);
        copyCmd.scissorY = static_cast<int32_t>(srcBox->top);
        copyCmd.scissorW = static_cast<int32_t>(srcBox->right > srcBox->left ? (srcBox->right - srcBox->left) : sW);
        copyCmd.scissorH = static_cast<int32_t>(srcBox->bottom > srcBox->top ? (srcBox->bottom - srcBox->top) : sH);
    } else {
        copyCmd.scissorX = 0;
        copyCmd.scissorY = 0;
        copyCmd.scissorW = static_cast<int32_t>(sW);
        copyCmd.scissorH = static_cast<int32_t>(sH);
    }

    sRegistry.setGpuOwned(pDstResource, true);
    {
        std::lock_guard<std::mutex> lock(sSpriteMutex);
        sActiveDrawingMeshes.push_back(std::move(copyCmd));
    }
}

static void WINAPI mock_Context_CopyResource(void*, void* pDstResource, void* pSrcResource) {
    static uint32_t sCopyResLog = 0;
    if (++sCopyResLog <= 15 || sCopyResLog % 300 == 1) {
        LOGI("ID3D11DeviceContext::CopyResource #%u called: Dst=%p, Src=%p", sCopyResLog, pDstResource, pSrcResource);
    }
    if (!pDstResource || !pSrcResource) return;

    sgre::TextureDesc srcDesc{};
    bool isSrcRenderTarget = false;
    bool isGpuBacked = isGpuBackedResource(pSrcResource, srcDesc, isSrcRenderTarget);

    sgre::TextureDesc dstDesc{};
    if (isGpuBacked && sRegistry.getTextureDesc(pDstResource, dstDesc)) {
        enqueueGpuTextureCopy(pDstResource, dstDesc, 0, 0, pSrcResource, srcDesc, nullptr);
        return;
    }

    if (!isGpuBacked && sRegistry.copyResource(pDstResource, pSrcResource)) {
        sRegistry.touchTexture(pDstResource);
    }
}

static void WINAPI mock_Context_CopySubresourceRegion(
    void*, void* pDstResource, uint32_t dstSubresource, uint32_t dstX, uint32_t dstY, uint32_t dstZ,
    void* pSrcResource, uint32_t srcSubresource, const void* pSrcBox) {
    if (!pDstResource || !pSrcResource) return;

    const auto* box = reinterpret_cast<const sgre::UpdateBox*>(pSrcBox);
    sgre::TextureDesc srcDesc{};
    bool isSrcRenderTarget = false;
    bool isGpuBacked = isGpuBackedResource(pSrcResource, srcDesc, isSrcRenderTarget);

    sgre::TextureDesc dstDesc{};
    if (isGpuBacked && sRegistry.getTextureDesc(pDstResource, dstDesc)) {
        enqueueGpuTextureCopy(pDstResource, dstDesc, dstX, dstY, pSrcResource, srcDesc, box);
        return;
    }

    if (!isGpuBacked && sRegistry.copySubresourceRegion(pDstResource, dstSubresource, dstX, dstY, dstZ, pSrcResource, srcSubresource, box)) {
        sRegistry.touchTexture(pDstResource);
    }
}

static void WINAPI mock_Context_VSSetConstantBuffers(void*, uint32_t startSlot, uint32_t numBuffers, void* const* ppConstantBuffers) {
    static uint32_t sVsCbLog = 0;
    if (++sVsCbLog <= 5) {
        LOGI("Context_VSSetConstantBuffers #%u: startSlot=%u, num=%u, buf0=%p",
             sVsCbLog, startSlot, numBuffers, (ppConstantBuffers && numBuffers > 0) ? ppConstantBuffers[0] : nullptr);
    }
    if (ppConstantBuffers != nullptr && numBuffers > 0) {
        for (uint32_t i = 0; i < numBuffers && (startSlot + i) < 16; ++i) {
            sBoundConstantBuffersVS[startSlot + i] = ppConstantBuffers[i];
        }
    }
}

static void WINAPI mock_Context_PSSetConstantBuffers(void*, uint32_t startSlot, uint32_t numBuffers, void* const* ppConstantBuffers) {
    static uint32_t sPsCbLog = 0;
    if (++sPsCbLog <= 5) {
        LOGI("Context_PSSetConstantBuffers #%u: startSlot=%u, num=%u, buf0=%p",
             sPsCbLog, startSlot, numBuffers, (ppConstantBuffers && numBuffers > 0) ? ppConstantBuffers[0] : nullptr);
    }
    if (ppConstantBuffers != nullptr && numBuffers > 0) {
        for (uint32_t i = 0; i < numBuffers && (startSlot + i) < 16; ++i) {
            sBoundConstantBuffersPS[startSlot + i] = ppConstantBuffers[i];
        }
    }
}

static void WINAPI mock_Context_PSSetShaderResources(void*, uint32_t startSlot, uint32_t numViews, void* const* ppShaderResourceViews) {
    if (ppShaderResourceViews != nullptr && numViews > 0) {
        for (uint32_t i = 0; i < numViews; ++i) {
            uint32_t slot = startSlot + i;
            if (slot < 8) {
                void* viewPtr = ppShaderResourceViews[i];
                sBoundShaderResourceViews[slot] = viewPtr;
                if (viewPtr != nullptr) {
                    auto* view = reinterpret_cast<const MockViewCOM*>(viewPtr);
                    void* res = (view->pResource != nullptr) ? view->pResource : viewPtr;
                    sBoundTextureResources[slot] = res;
                    if (slot == 1 && res != nullptr) {
                        sgre::TextureDesc d{};
                        if (sRegistry.getTextureDesc(res, d) && d.format == sgre::kDxgiA8Unorm) {
                            sLastOffscreenMaskResource = res;
                        }
                    }
                } else {
                    sBoundTextureResources[slot] = nullptr;
                }
            }
        }



        if (startSlot == 0) {
            if (ppShaderResourceViews[0] != nullptr) {
                sActiveShaderResourceView = ppShaderResourceViews[0];
                auto* view0 = reinterpret_cast<const MockViewCOM*>(ppShaderResourceViews[0]);
                sActiveTextureResource = (view0->pResource != nullptr) ? view0->pResource : ppShaderResourceViews[0];
            } else {
                sActiveShaderResourceView = nullptr;
                sActiveTextureResource = nullptr;
            }
        }

        static uint32_t sPsSrvLog = 0;
        if (++sPsSrvLog <= 5) {
            sgre::TextureDesc d0{}, d1{};
            uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
            if (sBoundTextureResources[0] && sRegistry.getTextureDesc(sBoundTextureResources[0], d0)) { w0 = d0.width; h0 = d0.height; }
            if (sBoundTextureResources[1] && sRegistry.getTextureDesc(sBoundTextureResources[1], d1)) { w1 = d1.width; h1 = d1.height; }
            LOGI("PSSetShaderResources: start=%u, num=%u | Slot0: %p (%ux%u) | Slot1: %p (%ux%u)",
                 startSlot, numViews, sBoundTextureResources[0], w0, h0, sBoundTextureResources[1], w1, h1);
        }
    } else if (numViews == 0) {
        // Unbind tất cả các slot bắt đầu từ startSlot
        for (uint32_t s = startSlot; s < 8; ++s) {
            sBoundShaderResourceViews[s] = nullptr;
            sBoundTextureResources[s] = nullptr;
        }
    }
}

static void WINAPI mock_Context_PSSetShader(void*, void* pShader, void* const*, uint32_t) {
    if (pShader != nullptr) {
        auto* ms = reinterpret_cast<MockShader*>(pShader);
        if (ms >= &sPixelShaders[0] && ms < &sPixelShaders[2080]) {
            sCurrentPixelShaderId = ms->id;
            static uint32_t sPsLog = 0;
            if (++sPsLog <= 5) {
                LOGI("PSSetShader: ShaderId=%u", sCurrentPixelShaderId);
            }
        }
    }
}

static void WINAPI mock_Context_PSSetSamplers(void*, uint32_t, uint32_t, void* const*) {}

static void WINAPI mock_Context_VSSetShader(void*, void* pShader, void* const*, uint32_t) {
    if (pShader != nullptr) {
        auto* ms = reinterpret_cast<MockShader*>(pShader);
        if (ms >= &sVertexShaders[0] && ms < &sVertexShaders[16]) {
            sCurrentVertexShaderId = ms->id;
        }
    }
    static uint32_t sVsSetLog = 0;
    if (++sVsSetLog <= 5) {
        LOGI("Context_VSSetShader #%u: pShader=%p, vsId=%u", sVsSetLog, pShader, sCurrentVertexShaderId);
    }
}

static void WINAPI mock_Context_IASetVertexBuffers(void*, uint32_t startSlot, uint32_t numBuffers, void* const* ppVertexBuffers, const uint32_t* pStrides, const uint32_t* pOffsets) {
    static uint32_t sVbLogCount = 0;
    if (numBuffers > 0 && ppVertexBuffers && ppVertexBuffers[0]) {
        void* newVb = ppVertexBuffers[0];
        if (sCurrentVertexBuffer != newVb) {
            if (sCurrentVertexBuffer != nullptr) {
                mock_COM_Release(const_cast<void*>(sCurrentVertexBuffer));
            }
            sCurrentVertexBuffer = newVb;
            mock_COM_AddRef(const_cast<void*>(sCurrentVertexBuffer));
        }
        if (pStrides && pStrides[0] > 0) {
            sCurrentVertexStride = pStrides[0];
        }
        sCurrentVertexOffset = (pOffsets && pOffsets[0] > 0) ? pOffsets[0] : 0;
        if (++sVbLogCount <= 5) {
            sgre::BufferDesc bDesc{};
            uint32_t bWidth = 0;
            if (sRegistry.getBufferDesc(sCurrentVertexBuffer, bDesc)) {
                bWidth = bDesc.byteWidth;
            }
            LOGI("IASetVertexBuffers: Vb=%p (Width=%u), Stride=%u, Offset=%u, Slot=%u, numBuffers=%u",
                 sCurrentVertexBuffer, bWidth, sCurrentVertexStride, sCurrentVertexOffset, startSlot, numBuffers);
        }
    }
}

static void WINAPI mock_Context_IASetIndexBuffer(void*, void* pIndexBuffer, uint32_t format, uint32_t offset) {
    if (pIndexBuffer) {
        if (sCurrentIndexBuffer != pIndexBuffer) {
            if (sCurrentIndexBuffer != nullptr) {
                mock_COM_Release(const_cast<void*>(sCurrentIndexBuffer));
            }
            sCurrentIndexBuffer = pIndexBuffer;
            mock_COM_AddRef(const_cast<void*>(sCurrentIndexBuffer));
        }
        sCurrentIndexFormat = format;
        sCurrentIndexOffset = offset;
        static uint32_t sIbLogCount = 0;
        if (++sIbLogCount <= 5) {
            sgre::BufferDesc bDesc{};
            uint32_t bWidth = 0;
            if (sRegistry.getBufferDesc(sCurrentIndexBuffer, bDesc)) {
                bWidth = bDesc.byteWidth;
            }
            LOGI("IASetIndexBuffer: Ib=%p (Width=%u), Format=%u, Offset=%u", sCurrentIndexBuffer, bWidth, format, offset);
        }
    } else {
        if (sCurrentIndexBuffer != nullptr) {
            mock_COM_Release(const_cast<void*>(sCurrentIndexBuffer));
            sCurrentIndexBuffer = nullptr;
        }
        sCurrentIndexOffset = 0;
    }
}

static void WINAPI mock_Context_IASetInputLayout(void*, void* pLayout) {
    sCurrentInputLayout = reinterpret_cast<MockInputLayoutCOM*>(pLayout);
    static uint32_t sIlLog = 0;
    if (++sIlLog <= 5) {
        LOGI("Context_IASetInputLayout #%u: pLayout=%p (posOff=%d, colOff=%d, tex0Off=%d, stride=%u)",
             sIlLog, pLayout,
             sCurrentInputLayout ? sCurrentInputLayout->posOffset : -99,
             sCurrentInputLayout ? sCurrentInputLayout->colorOffset : -99,
             sCurrentInputLayout ? sCurrentInputLayout->tex0Offset : -99,
             sCurrentInputLayout ? sCurrentInputLayout->stride : 0);
    }
}
static void WINAPI mock_Context_IASetPrimitiveTopology(void*, uint32_t topology) {
    static uint32_t sTopoLog = 0;
    if (sCurrentPrimitiveTopology != topology && ++sTopoLog <= 5) {
        LOGI("IASetPrimitiveTopology: changed from %u to %u", sCurrentPrimitiveTopology, topology);
    }
    sCurrentPrimitiveTopology = topology;
}
static void WINAPI mock_Context_VSSetShaderResources(void*, uint32_t startSlot, uint32_t numViews, void* const* ppShaderResourceViews) {
    if (ppShaderResourceViews != nullptr && numViews > 0) {
        for (uint32_t i = 0; i < numViews; ++i) {
            uint32_t slot = startSlot + i;
            if (slot < 8) {
                void* viewPtr = ppShaderResourceViews[i];
                if (viewPtr != nullptr) {
                    auto* view = reinterpret_cast<const MockViewCOM*>(viewPtr);
                    void* res = (view->pResource != nullptr) ? view->pResource : viewPtr;
                    if (slot == 0 && sBoundTextureResources[0] == nullptr) {
                        sBoundTextureResources[0] = res;
                    }
                }
            }
        }
    }
}
static void WINAPI mock_Context_VSSetSamplers(void*, uint32_t, uint32_t, void* const*) {}
static void WINAPI mock_Context_OMSetRenderTargets(void*, uint32_t numViews, void* const* ppRenderTargetViews, void* pDepthStencilView) {
    if (ppRenderTargetViews != nullptr && numViews > 0 && ppRenderTargetViews[0] != nullptr) {
        auto* rtv = reinterpret_cast<const MockRenderTargetViewCOM*>(ppRenderTargetViews[0]);
        sCurrentRenderTargetResource = rtv->pResource;
    } else {
        sCurrentRenderTargetResource = nullptr;
    }
    // Tuân thủ quy tắc chống xung đột đọc-ghi D3D11: tự động gỡ bind SRV nếu tài nguyên đang được gắn làm Render Target
    if (sCurrentRenderTargetResource != nullptr && sCurrentRenderTargetResource != &gMockTexture2D) {
        for (uint32_t slot = 0; slot < 8; ++slot) {
            if (sBoundTextureResources[slot] == sCurrentRenderTargetResource) {
                sBoundShaderResourceViews[slot] = nullptr;
                sBoundTextureResources[slot] = nullptr;
            }
        }
    }
    static uint32_t sOmRtLog = 0;
    if (++sOmRtLog <= 5) {
        LOGI("OMSetRenderTargets: numViews=%u, targetRes=%p, pDepthStencilView=%p",
             numViews, sCurrentRenderTargetResource, pDepthStencilView);
    }
}

static void WINAPI mock_Context_OMSetBlendState(void*, void* pBlendState, const float blendFactor[4], uint32_t sampleMask) {
    sCurrentBlendState = reinterpret_cast<const MockBlendStateCOM*>(pBlendState);
    if (blendFactor != nullptr) {
        sCurrentBlendFactor[0] = blendFactor[0];
        sCurrentBlendFactor[1] = blendFactor[1];
        sCurrentBlendFactor[2] = blendFactor[2];
        sCurrentBlendFactor[3] = blendFactor[3];
    } else {
        sCurrentBlendFactor[0] = 1.0f;
        sCurrentBlendFactor[1] = 1.0f;
        sCurrentBlendFactor[2] = 1.0f;
        sCurrentBlendFactor[3] = 1.0f;
    }
    static uint32_t sBsLog = 0;
    if (++sBsLog <= 5 && sCurrentBlendState != nullptr) {
        const auto& rt0 = sCurrentBlendState->desc.renderTarget[0];
        LOGI("OMSetBlendState: state=%p, RT0 BlendEnable=%d, SrcRGB=%u, DstRGB=%u, OpRGB=%u, SrcA=%u, DstA=%u, OpA=%u, WriteMask=0x%02X",
             pBlendState, rt0.blendEnable, rt0.srcBlend, rt0.destBlend, rt0.blendOp,
             rt0.srcBlendAlpha, rt0.destBlendAlpha, rt0.blendOpAlpha, rt0.renderTargetWriteMask);
    }
}

static void WINAPI mock_Context_OMSetDepthStencilState(void*, void* pDepthStencilState, uint32_t stencilRef) {
    sCurrentDepthStencilState = reinterpret_cast<const MockDepthStencilStateCOM*>(pDepthStencilState);
    sCurrentStencilRef = stencilRef;
    static uint32_t sDssLog = 0;
    if (++sDssLog <= 5 && sCurrentDepthStencilState != nullptr) {
        LOGI("OMSetDepthStencilState: state=%p, Ref=%u, StencilEnable=%d, FrontFunc=%u, PassOp=%u, WriteMask=0x%02X",
             pDepthStencilState, stencilRef, sCurrentDepthStencilState->desc.stencilEnable,
             sCurrentDepthStencilState->desc.frontFace.stencilFunc,
             sCurrentDepthStencilState->desc.frontFace.stencilPassOp,
             sCurrentDepthStencilState->desc.stencilWriteMask);
    }
}

static void WINAPI mock_Context_RSSetState(void*, void* pRasterizerState) {
    sCurrentRasterizerState = reinterpret_cast<const MockRasterizerStateCOM*>(pRasterizerState);
    static uint32_t sRsLog = 0;
    if (++sRsLog <= 5 && sCurrentRasterizerState != nullptr) {
        LOGI("RSSetState: state=%p, ScissorEnable=%d, CullMode=%d",
             pRasterizerState, sCurrentRasterizerState->desc.scissorEnable, sCurrentRasterizerState->desc.cullMode);
    }
}

static void WINAPI mock_Context_ClearDepthStencilView(void*, void*, uint32_t clearFlags, float, uint8_t stencil) {
    static uint32_t sClearDsLog = 0;
    if (++sClearDsLog % 60 == 1) {
        LOGI("ClearDepthStencilView: flags=0x%x, StencilVal=%u", clearFlags, stencil);
    }
    if ((clearFlags & 0x2) != 0) {
        MeshDrawCommand clearCmd{};
        clearCmd.isClearCommand = 2;
        clearCmd.clearStencil = stencil;
        bool isOffscreen = (sCurrentRenderTargetResource != nullptr && sCurrentRenderTargetResource != &gMockTexture2D);
        clearCmd.renderTargetHandle = isOffscreen ? sCurrentRenderTargetResource : nullptr;
        clearCmd.renderTargetWidth = gCurrentWidth;
        clearCmd.renderTargetHeight = gCurrentHeight;
        if (isOffscreen) {
            sgre::TextureDesc rtDesc{};
            if (sRegistry.getTextureDesc(sCurrentRenderTargetResource, rtDesc)) {
                clearCmd.renderTargetWidth = rtDesc.width;
                clearCmd.renderTargetHeight = rtDesc.height;
            }
        }
        std::lock_guard<std::mutex> lock(sSpriteMutex);
        sActiveDrawingMeshes.push_back(std::move(clearCmd));
    }
}

static void WINAPI mock_Context_ClearRenderTargetView(void*, void* pRenderTargetView, const float colorRGBA[4]) {
    const void* targetRes = nullptr;
    if (pRenderTargetView != nullptr) {
        auto* rtv = reinterpret_cast<const MockRenderTargetViewCOM*>(pRenderTargetView);
        targetRes = rtv->pResource;
    }
    bool isBackbuffer = (targetRes == nullptr || targetRes == &gMockTexture2D);
    if (isBackbuffer) {
        {
            std::lock_guard<std::mutex> sLock(sSpriteMutex);
            sActiveDrawingSprites.clear();
        }
        if (colorRGBA) {
            std::lock_guard<std::mutex> lock(sFrameMutex);
            sCurrentFrameState.clearColor[0] = colorRGBA[0];
            sCurrentFrameState.clearColor[1] = colorRGBA[1];
            sCurrentFrameState.clearColor[2] = colorRGBA[2];
            sCurrentFrameState.clearColor[3] = colorRGBA[3];
            static uint32_t clearCount = 0;
            if (++clearCount % 60 == 1) {
                LOGI("ClearRenderTargetView (Backbuffer): RGBA=(%.3f, %.3f, %.3f, %.3f)",
                     colorRGBA[0], colorRGBA[1], colorRGBA[2], colorRGBA[3]);
            }
        }
    } else {
        // Xóa nội dung Offscreen RenderTarget trên GPU (và bản sao CPU nếu là Mask A8_UNORM)
        uint8_t clearVal = colorRGBA ? static_cast<uint8_t>(colorRGBA[3] * 255.0f) : 0;
        sOffscreenClearVals[targetRes] = clearVal;
        sgre::TextureDesc tDesc{};
        if (sRegistry.getTextureDesc(targetRes, tDesc)) {
            if (tDesc.format == sgre::kDxgiA8Unorm) {
                sLastOffscreenMaskResource = targetRes;
                sMaskNeedsReset.store(false);
            }
            sgre::MappedView view{};
            if (sRegistry.map(const_cast<void*>(targetRes), view) && view.data != nullptr) {
                size_t totalPixels = static_cast<size_t>(tDesc.width) * tDesc.height;
                if (clearVal == 0) {
                    memset(view.data, 0, totalPixels * 4);
                } else {
                    for (size_t p = 0; p < totalPixels; ++p) {
                        view.data[p * 4 + 0] = clearVal;
                        view.data[p * 4 + 1] = clearVal;
                        view.data[p * 4 + 2] = clearVal;
                        view.data[p * 4 + 3] = clearVal;
                    }
                }
            }
            // Không gọi sRegistry.touchTexture(targetRes) vì FBO texture được GPU xóa qua clearCmd.
            MeshDrawCommand clearCmd{};
            clearCmd.isClearCommand = 1;
            clearCmd.renderTargetHandle = targetRes;
            clearCmd.renderTargetWidth = tDesc.width;
            clearCmd.renderTargetHeight = tDesc.height;
            if (colorRGBA) {
                memcpy(clearCmd.clearColor, colorRGBA, sizeof(clearCmd.clearColor));
            } else {
                memset(clearCmd.clearColor, 0, sizeof(clearCmd.clearColor));
            }
            {
                std::lock_guard<std::mutex> lock(sSpriteMutex);
                sActiveDrawingMeshes.push_back(std::move(clearCmd));
            }

            static uint32_t sOffscreenClearLog = 0;
            if (++sOffscreenClearLog <= 5) {
                LOGI("ClearRenderTargetView (Offscreen %ux%u format=%u): val=0x%02X",
                     tDesc.width, tDesc.height, tDesc.format, clearVal);
            }
        }
    }
}

bool D3D11Bridge_GetActiveTextureInfo(uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData, uint32_t* outRevision) {
    const uint8_t* rgbaData = nullptr;
    if (!D3D11Bridge_GetTextureInfoByHandle(sActiveTextureResource, outWidth, outHeight, &rgbaData, nullptr, outRevision)) {
        return false;
    }
    if (rgbaData == nullptr) return false;
    if (outRgbaData) *outRgbaData = rgbaData;
    return true;
}

bool D3D11Bridge_GetTextureInfoByHandle(const void* handle, uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData, uint32_t* outFormat, uint32_t* outRevision, uint64_t* outGenerationId) {
    if (handle == nullptr) return false;

    sgre::TextureDesc desc{};
    if (!sRegistry.getTextureDesc(handle, desc)) return false;

    sgre::MappedView view{};
    bool hasCpuData = sRegistry.map(handle, view);
    if (!hasCpuData && (desc.bindFlags & kD3d11BindRenderTarget) == 0 &&
        desc.usage != 0 && !sRegistry.isGpuOwned(handle)) {
        return false;
    }

    if (outWidth) *outWidth = desc.width;
    if (outHeight) *outHeight = desc.height;
    if (outRgbaData) *outRgbaData = hasCpuData ? view.data : nullptr;
    if (outFormat) *outFormat = desc.format;
    if (outRevision) *outRevision = desc.revision;
    if (outGenerationId) *outGenerationId = desc.generationId;
    return true;
}

bool D3D11Bridge_AcquireTextureSnapshot(const void* handle, TextureCpuSnapshot& out) {
    if (handle == nullptr) return false;
    sgre::TextureSnapshot snap{};
    if (!sRegistry.acquireTextureSnapshot(handle, snap)) return false;

    const bool hasCpuData = snap.pixels && !snap.pixels->empty();
    const bool isRt = (snap.desc.bindFlags & kD3d11BindRenderTarget) != 0;
    if (!hasCpuData && !isRt && snap.desc.usage != 0 && !snap.hasGpuContents && !snap.isUploadedToGpu) {
        return false;
    }

    out.width = snap.desc.width;
    out.height = snap.desc.height;
    out.format = snap.desc.format;
    out.revision = snap.desc.revision;
    out.generationId = snap.desc.generationId;
    out.isRenderTarget = isRt;
    out.isGpuOwned = snap.hasGpuContents;
    out.isUploadedToGpu = snap.isUploadedToGpu;
    out.isCompressedBc7 = snap.desc.isCompressedBc7;
    out.pixels = std::move(snap.pixels);
    return true;
}

bool D3D11Bridge_WaitAndConsumePreuploadRequests(std::vector<TextureUploadRequest>& out, uint32_t timeoutMs) {
    out.clear();
    std::unique_lock<std::mutex> lock(sPreuploadMutex);
    if (sPreuploadQueue.empty()) {
        if (timeoutMs == 0) return false;
        sPreuploadCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
            return !sPreuploadQueue.empty() || sPreuploadWake.load();
        });
        sPreuploadWake.store(false);
        if (sPreuploadQueue.empty()) return false;
    }
    out.swap(sPreuploadQueue);
    return true;
}

void D3D11Bridge_ConsumePreuploadRequests(std::vector<TextureUploadRequest>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(sPreuploadMutex);
    if (!sPreuploadQueue.empty()) {
        out.swap(sPreuploadQueue);
    }
}

void D3D11Bridge_WakePreuploadThread() {
    {
        std::lock_guard<std::mutex> lock(sPreuploadMutex);
        sPreuploadWake.store(true);
    }
    sPreuploadCond.notify_all();
}

void D3D11Bridge_MarkTextureUploaded(const void* handle) {
    if (handle) {
        sRegistry.markUploadedToGpu(handle);
    }
}

bool D3D11Bridge_PreserveGpuTextureToCpu(const void* handle, const uint8_t* rgbaData, size_t byteSize) {
    if (!handle || !rgbaData || byteSize == 0) return false;
    return sRegistry.preserveGpuTextureToCpu(handle, rgbaData, byteSize);
}

void D3D11Bridge_ResetGpuUploadState() {
    sRegistry.resetAllUploadedFlags();
}

uint64_t D3D11Bridge_GetCpuMemoryBytes() {
    return sRegistry.stats().cpuBytes;
}

void D3D11Bridge_SetSupportsBptcCompression(bool supported) {
    sRegistry.setBptcEnabled(supported);
}

bool D3D11Bridge_SupportsBptcCompression() {
    return sRegistry.isBptcEnabled();
}

bool D3D11Bridge_EnsureTextureDecompressed(const void* handle) {
    return sRegistry.ensureTextureDecompressed(handle);
}

bool D3D11Bridge_IsStagingTexture(const void* handle) {
    if (handle == nullptr) return false;
    sgre::TextureDesc desc{};
    if (!sRegistry.getTextureDesc(handle, desc)) return false;
    return desc.usage == kD3d11UsageStaging || (desc.cpuAccessFlags & kD3d11CpuAccessRead) != 0;
}

bool D3D11Bridge_IsRenderTargetHandle(const void* handle) {
    if (handle == nullptr) return false;
    sgre::TextureDesc desc{};
    if (!sRegistry.getTextureDesc(handle, desc)) return false;
    return (desc.bindFlags & kD3d11BindRenderTarget) != 0;
}

bool D3D11Bridge_HasGpuContents(const void* handle) {
    if (handle == nullptr) return false;
    return sRegistry.isGpuOwned(handle) || D3D11Bridge_IsRenderTargetHandle(handle);
}

void D3D11Bridge_SetRenderLoopActive(bool active) {
    sRenderLoopActive.store(active);
    if (!active) {
        {
            std::lock_guard<std::mutex> lock(sFlushMutex);
            sFlushPending = false;
            sFlushDone = true;
            sFlushSucceeded = false;
        }
        sFlushCond.notify_all();
        sFrameAcknowledgeCond.notify_all();
        sSpriteConsumedCond.notify_all();
        D3D11Bridge_WakePreuploadThread();
    } else {
        std::lock_guard<std::mutex> lock(sPreuploadMutex);
        sPreuploadWake.store(false);
    }
}

bool D3D11Bridge_ConsumeFlushRequest(GpuFlushRequest& outReq) {
    std::lock_guard<std::mutex> lock(sFlushMutex);
    if (!sFlushPending || sFlushDone) {
        return false;
    }
    outReq = std::move(sPendingFlushRequest);
    sFlushPending = false;
    return true;
}

void D3D11Bridge_CompleteFlushRequest(bool success) {
    {
        std::lock_guard<std::mutex> lock(sFlushMutex);
        sFlushSucceeded = success;
        sFlushDone = true;
    }
    sFlushCond.notify_all();
}

void D3D11Bridge_ConsumeReleasedHandles(std::vector<const void*>& outHandles) {
    std::lock_guard<std::mutex> lock(sDeferredMutex);
    outHandles = std::move(sDeferredReleaseList);
    sDeferredReleaseList.clear();
}

void D3D11Bridge_FinalizeRelease(const void* handle) {
    (void)handle;
}

size_t D3D11Bridge_GetSpriteBatchInfo(SpriteDrawCommand* outCommands, size_t maxCommands) {
    std::lock_guard<std::mutex> lock(sSpriteMutex);
    size_t count = std::min(sReadyFrameSprites.size(), maxCommands);
    if (outCommands != nullptr && count > 0) {
        std::memcpy(outCommands, sReadyFrameSprites.data(), count * sizeof(SpriteDrawCommand));
    }
    return count;
}

size_t D3D11Bridge_SwapFrameMeshes(std::vector<MeshDrawCommand>& outMeshes) {
    std::lock_guard<std::mutex> lock(sSpriteMutex);
    outMeshes.clear();
    outMeshes.swap(sReadyFrameMeshes);
    return outMeshes.size();
}

bool D3D11Bridge_WaitForNextFrame(RenderFrameState* outState, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(sFrameMutex);
    if (!sNewFrameAvailable) {
        if (timeoutMs == 0) return false;
        sFrameCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), []{
            return sNewFrameAvailable || sWakeRequested.load();
        });
        sWakeRequested.store(false);
        if (!sNewFrameAvailable) return false;
    }
    sNewFrameAvailable = false;
    if (outState) {
        *outState = sCurrentFrameState;
    }
    return true;
}

void D3D11Bridge_WakeRenderThread() {
    {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sWakeRequested.store(true);
    }
    sFrameCond.notify_all();
}

void* D3D11Bridge_GetMockTexture2D() {
    return &gMockTexture2D;
}

void D3D11Bridge_GetLatestFrameState(RenderFrameState* outState) {
    std::lock_guard<std::mutex> lock(sFrameMutex);
    if (outState) {
        *outState = sCurrentFrameState;
    }
}

void D3D11Bridge_NotifyFramePresented() {
    {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sFramePendingAck.store(false);
    }
    sFrameAcknowledgeCond.notify_all();
}

void D3D11Bridge_NotifySpritesConsumed() {
    {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sSpritePendingConsume.store(false);
    }
    sSpriteConsumedCond.notify_all();
}

#pragma pack(push, 8)
struct D3D11_SHADER_DESC_64 {
    uint32_t Version;
    const char* Creator;
    uint32_t Flags;
    uint32_t ConstantBuffers;
    uint32_t BoundResources;
    uint32_t InputParameters;
    uint32_t OutputParameters;
    uint32_t InstructionCount;
    uint32_t TempRegisterCount;
    uint32_t TempArrayCount;
    uint32_t DefCount;
    uint32_t DclCount;
    uint32_t TextureNormalInstructions;
    uint32_t TextureLoadInstructions;
    uint32_t TextureCompInstructions;
    uint32_t TextureBiasInstructions;
    uint32_t TextureGradientInstructions;
    uint32_t FloatInstructionCount;
    uint32_t IntInstructionCount;
    uint32_t UintInstructionCount;
    uint32_t StaticFlowControlCount;
    uint32_t DynamicFlowControlCount;
    uint32_t MacroInstructionCount;
    uint32_t ArrayInstructionCount;
    uint32_t CutInstructionCount;
    uint32_t EmitInstructionCount;
    uint32_t GSOutputTopology;
    uint32_t GSInstanceCount;
    uint32_t ControlPoints;
    uint32_t HSOutputPrimitive;
    uint32_t HSPartitioning;
    uint32_t TessellatorDomain;
    uint32_t cBarrierInstructions;
    uint32_t cInterlockedInstructions;
    uint32_t cTextureStoreInstructions;
};

struct D3D11_SHADER_INPUT_BIND_DESC_64 {
    const char* Name;
    uint32_t Type;          // 0 = CBuffer, 2 = Texture
    uint32_t ReturnType;
    uint32_t Dimension;
    uint32_t NumSamples;
    uint32_t BindPoint;
    uint32_t BindCount;
    uint32_t uFlags;
};

struct D3D11_SHADER_BUFFER_DESC_64 {
    const char* Name;
    uint32_t Type;          // 0 = D3D_CT_CBUFFER
    uint32_t Variables;
    uint32_t Size;
    uint32_t uFlags;
};

struct D3D11_SHADER_VARIABLE_DESC_64 {
    const char* Name;
    uint32_t StartOffset;
    uint32_t Size;
    uint32_t uFlags;
    const void* DefaultValue;
    uint32_t StartTexture;
    uint32_t TextureSize;
    uint32_t StartSampler;
    uint32_t SamplerSize;
};

struct D3D11_SHADER_TYPE_DESC_64 {
    uint32_t Class;         // 1 = Vector, 3 = Matrix
    uint32_t Type;          // 3 = Float
    uint32_t Rows;
    uint32_t Columns;
    uint32_t Elements;      // 1 (bắt buộc != 0)
    uint32_t Members;
    uint32_t Offset;
    const char* Name;
};
#pragma pack(pop)

struct MockTypeInstance {
    uint64_t* vtable;
    D3D11_SHADER_TYPE_DESC_64 desc;
};

struct MockVariableInstance {
    uint64_t* vtable;
    D3D11_SHADER_VARIABLE_DESC_64 desc;
    MockTypeInstance typeInst;
};

static sgre::DxbcReflectionData gCurrentReflectionData;
static MockVariableInstance gMockVariables[16];
static MockVariableInstance gFallbackInvalidVar;

static void syncMockVariablesFromCurrentData() {
    gFallbackInvalidVar.vtable = gShaderReflectionVariableVtable;
    memset(&gFallbackInvalidVar.desc, 0, sizeof(gFallbackInvalidVar.desc));
    gFallbackInvalidVar.desc.Name = "";
    gFallbackInvalidVar.typeInst.vtable = gShaderReflectionTypeVtable;
    memset(&gFallbackInvalidVar.typeInst.desc, 0, sizeof(gFallbackInvalidVar.typeInst.desc));
    gFallbackInvalidVar.typeInst.desc.Class = 1;
    gFallbackInvalidVar.typeInst.desc.Type = 3;
    gFallbackInvalidVar.typeInst.desc.Rows = 1;
    gFallbackInvalidVar.typeInst.desc.Columns = 4;
    gFallbackInvalidVar.typeInst.desc.Elements = 1;
    gFallbackInvalidVar.typeInst.desc.Name = "float4";

    if (gCurrentReflectionData.constantBuffers.empty()) {
        return;
    }
    const auto& vars = gCurrentReflectionData.constantBuffers[0].variables;
    size_t count = std::min(vars.size(), static_cast<size_t>(16));
    for (size_t i = 0; i < count; ++i) {
        gMockVariables[i].vtable = gShaderReflectionVariableVtable;
        memset(&gMockVariables[i].desc, 0, sizeof(D3D11_SHADER_VARIABLE_DESC_64));
        gMockVariables[i].desc.Name = vars[i].name.c_str();
        gMockVariables[i].desc.StartOffset = vars[i].startOffset;
        gMockVariables[i].desc.Size = vars[i].size;
        gMockVariables[i].desc.uFlags = vars[i].uFlags;

        gMockVariables[i].typeInst.vtable = gShaderReflectionTypeVtable;
        memset(&gMockVariables[i].typeInst.desc, 0, sizeof(D3D11_SHADER_TYPE_DESC_64));
        gMockVariables[i].typeInst.desc.Class = vars[i].typeInfo.varClass;
        gMockVariables[i].typeInst.desc.Type = vars[i].typeInfo.varType;
        gMockVariables[i].typeInst.desc.Rows = vars[i].typeInfo.rows;
        gMockVariables[i].typeInst.desc.Columns = vars[i].typeInfo.columns;
        gMockVariables[i].typeInst.desc.Elements = vars[i].typeInfo.elements > 0 ? vars[i].typeInfo.elements : 1;
        gMockVariables[i].typeInst.desc.Members = vars[i].typeInfo.members;
        gMockVariables[i].typeInst.desc.Offset = vars[i].typeInfo.offset;
        gMockVariables[i].typeInst.desc.Name = vars[i].typeInfo.name.c_str();
    }
}

static int64_t WINAPI mock_Reflection_GetDesc(void*, void* pDesc) {
    if (pDesc) {
        memset(pDesc, 0, sizeof(D3D11_SHADER_DESC_64));
        auto* desc = reinterpret_cast<D3D11_SHADER_DESC_64*>(pDesc);
        desc->Version = gCurrentReflectionData.version;
        desc->Creator = gCurrentReflectionData.creator.c_str();
        desc->Flags = gCurrentReflectionData.flags;
        desc->ConstantBuffers = static_cast<uint32_t>(gCurrentReflectionData.constantBuffers.size());
        desc->BoundResources = static_cast<uint32_t>(gCurrentReflectionData.boundResources.size());
        desc->InstructionCount = 10;
    }
    return 0; // S_OK
}

static int64_t WINAPI mock_Reflection_GetResourceBindingDesc(void*, uint32_t resourceIndex, void* pDesc) {
    if (pDesc) {
        memset(pDesc, 0, sizeof(D3D11_SHADER_INPUT_BIND_DESC_64));
        auto* desc = reinterpret_cast<D3D11_SHADER_INPUT_BIND_DESC_64*>(pDesc);
        if (resourceIndex < gCurrentReflectionData.boundResources.size()) {
            const auto& rb = gCurrentReflectionData.boundResources[resourceIndex];
            desc->Name = rb.name.c_str();
            desc->Type = rb.type;
            desc->ReturnType = rb.returnType;
            desc->Dimension = rb.dimension;
            desc->NumSamples = rb.numSamples;
            desc->BindPoint = rb.bindPoint;
            desc->BindCount = rb.bindCount;
            desc->uFlags = rb.uFlags;
        }
    }
    return 0; // S_OK
}

static void* WINAPI mock_Reflection_GetConstantBufferByIndex(void*, uint32_t index) {
    if (index < gCurrentReflectionData.constantBuffers.size()) {
        return &gMockShaderConstantBuffer;
    }
    return nullptr;
}

static void* WINAPI mock_Reflection_GetConstantBufferByName(void*, const char* name) {
    if (!gCurrentReflectionData.constantBuffers.empty() && name != nullptr) {
        if (gCurrentReflectionData.constantBuffers[0].name == name) {
            return &gMockShaderConstantBuffer;
        }
    }
    return &gMockShaderConstantBuffer;
}

static int64_t WINAPI mock_Reflection_GetParameterDesc(void*, uint32_t, void* pDesc) {
    if (pDesc) memset(pDesc, 0, 128);
    return 0; // S_OK
}

static int64_t WINAPI mock_ConstantBuffer_GetDesc(void*, void* pDesc) {
    if (pDesc) {
        memset(pDesc, 0, sizeof(D3D11_SHADER_BUFFER_DESC_64));
        auto* desc = reinterpret_cast<D3D11_SHADER_BUFFER_DESC_64*>(pDesc);
        if (!gCurrentReflectionData.constantBuffers.empty()) {
            const auto& cb = gCurrentReflectionData.constantBuffers[0];
            desc->Name = cb.name.c_str();
            desc->Type = cb.type;
            desc->Variables = static_cast<uint32_t>(cb.variables.size());
            desc->Size = cb.size;
            desc->uFlags = cb.uFlags;
        } else {
            desc->Name = "$Globals";
            desc->Type = 0;
            desc->Variables = 0;
            desc->Size = 0;
        }
    }
    return 0; // S_OK
}

static void* WINAPI mock_ConstantBuffer_GetVariableByIndex(void*, uint32_t index) {
    if (!gCurrentReflectionData.constantBuffers.empty()) {
        size_t count = std::min(gCurrentReflectionData.constantBuffers[0].variables.size(), static_cast<size_t>(16));
        if (index < count) {
            return &gMockVariables[index];
        }
    }
    return &gFallbackInvalidVar;
}

static void* WINAPI mock_ConstantBuffer_GetVariableByName(void*, const char* name) {
    if (!gCurrentReflectionData.constantBuffers.empty() && name != nullptr) {
        size_t count = std::min(gCurrentReflectionData.constantBuffers[0].variables.size(), static_cast<size_t>(16));
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(gMockVariables[i].desc.Name, name) == 0) {
                return &gMockVariables[i];
            }
        }
    }
    return &gFallbackInvalidVar;
}

static int64_t WINAPI mock_Variable_GetDesc(void* thisPtr, void* pDesc) {
    if (pDesc) {
        memset(pDesc, 0, sizeof(D3D11_SHADER_VARIABLE_DESC_64));
        auto* varInst = reinterpret_cast<MockVariableInstance*>(thisPtr);
        if (varInst != nullptr) {
            *reinterpret_cast<D3D11_SHADER_VARIABLE_DESC_64*>(pDesc) = varInst->desc;
        }
    }
    return 0; // S_OK
}

static void* WINAPI mock_Variable_GetType(void* thisPtr) {
    auto* varInst = reinterpret_cast<MockVariableInstance*>(thisPtr);
    if (varInst != nullptr) {
        return &varInst->typeInst;
    }
    return &gFallbackInvalidVar.typeInst;
}

static int64_t WINAPI mock_Variable_GetConstantBuffer(void*, void** ppBuffer) {
    if (ppBuffer) *ppBuffer = &gMockShaderConstantBuffer;
    return 0; // S_OK
}

static int64_t WINAPI mock_Type_GetDesc(void* thisPtr, void* pDesc) {
    if (pDesc) {
        memset(pDesc, 0, sizeof(D3D11_SHADER_TYPE_DESC_64));
        auto* typeInst = reinterpret_cast<MockTypeInstance*>(thisPtr);
        if (typeInst != nullptr && typeInst->vtable == gShaderReflectionTypeVtable) {
            *reinterpret_cast<D3D11_SHADER_TYPE_DESC_64*>(pDesc) = typeInst->desc;
        } else {
            auto* desc = reinterpret_cast<D3D11_SHADER_TYPE_DESC_64*>(pDesc);
            desc->Class = 1;
            desc->Type = 3;
            desc->Rows = 4;
            desc->Columns = 4;
            desc->Elements = 1;
            desc->Name = "float4";
        }
    }
    return 0; // S_OK
}

static bool sDeviceSlotLogged[128] = {false};
static bool sContextSlotLogged[128] = {false};

template<size_t Slot>
struct SlotDispatcher {
    static int64_t WINAPI deviceSlot(void* thisPtr) {
        if (!sDeviceSlotLogged[Slot]) {
            sDeviceSlotLogged[Slot] = true;
            LOGI(">>> D3D11 Device::Slot[%zu] invoked on %p", Slot, thisPtr);
        }
        return 0; // S_OK
    }

    static int64_t WINAPI contextSlot(void* thisPtr) {
        if (!sContextSlotLogged[Slot]) {
            sContextSlotLogged[Slot] = true;
            LOGI(">>> D3D11 Context::Slot[%zu] invoked on %p", Slot, thisPtr);
        }
        return 0; // S_OK
    }
};

template<size_t... Is>
static void populateFallbackSlots(uint64_t* devTable, uint64_t* ctxTable, std::index_sequence<Is...>) {
    ((devTable[Is] = reinterpret_cast<uint64_t>(&SlotDispatcher<Is>::deviceSlot)), ...);
    ((ctxTable[Is] = reinterpret_cast<uint64_t>(&SlotDispatcher<Is>::contextSlot)), ...);
}

static void initVtablesOnce() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // 1. Khởi tạo tất cả vtable với telemetry thunks và mock_COM_Generic an toàn
    populateFallbackSlots(gDeviceVtable, gContextVtable, std::make_index_sequence<128>{});
    for (int i = 0; i < 32; ++i) {
        gDXGIDeviceVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gDXGIAdapterVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gDXGIFactoryVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gSwapChainVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gTexture2DVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gViewVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gGenericResourceVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gShaderReflectionVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
    }
    for (int i = 0; i < 16; ++i) {
        gMultithreadVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gShaderReflectionConstantBufferVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gShaderReflectionVariableVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
        gShaderReflectionTypeVtable[i] = reinterpret_cast<uint64_t>(&mock_COM_Generic);
    }

    // 2. IUnknown base slots (0, 1, 2) cho toàn bộ vtable
    uint64_t* allVtables[] = {
        gDeviceVtable, gContextVtable, gMultithreadVtable,
        gDXGIDeviceVtable, gDXGIAdapterVtable, gDXGIFactoryVtable,
        gSwapChainVtable, gTexture2DVtable, gViewVtable, gGenericResourceVtable,
        gShaderReflectionVtable
    };
    for (auto* vt : allVtables) {
        vt[0] = reinterpret_cast<uint64_t>(&mock_COM_QueryInterface);
        vt[1] = reinterpret_cast<uint64_t>(&mock_COM_AddRef);
        vt[2] = reinterpret_cast<uint64_t>(&mock_COM_Release);
    }

    // 3. ID3D11Multithread slots
    gMultithreadVtable[3] = reinterpret_cast<uint64_t>(&mock_MT_Enter);
    gMultithreadVtable[4] = reinterpret_cast<uint64_t>(&mock_MT_Leave);
    gMultithreadVtable[5] = reinterpret_cast<uint64_t>(&mock_MT_SetMultithreadProtected);
    gMultithreadVtable[6] = reinterpret_cast<uint64_t>(&mock_MT_GetMultithreadProtected);

    // 4. IDXGIDevice / IDXGIDevice1 slots
    gDXGIDeviceVtable[6] = reinterpret_cast<uint64_t>(&mock_COM_GetParent);
    gDXGIDeviceVtable[7] = reinterpret_cast<uint64_t>(&mock_DXGI_GetAdapter);
    gDXGIDeviceVtable[12] = reinterpret_cast<uint64_t>(&mock_DXGI_SetMaximumFrameLatency);
    gDXGIDeviceVtable[13] = reinterpret_cast<uint64_t>(&mock_DXGI_GetMaximumFrameLatency);

    // 5. IDXGIAdapter slots
    gDXGIAdapterVtable[6] = reinterpret_cast<uint64_t>(&mock_COM_GetParent);

    // 6. IDXGIFactory / IDXGIFactory2 slots
    gDXGIFactoryVtable[8] = reinterpret_cast<uint64_t>(&mock_Factory_MakeWindowAssociation);
    gDXGIFactoryVtable[10] = reinterpret_cast<uint64_t>(&mock_Factory_CreateSwapChain);
    gDXGIFactoryVtable[15] = reinterpret_cast<uint64_t>(&mock_Factory_CreateSwapChainForHwnd);

    // 7. IDXGISwapChain slots
    gSwapChainVtable[7] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetDevice);
    gSwapChainVtable[8] = reinterpret_cast<uint64_t>(&mock_SwapChain_Present);
    gSwapChainVtable[9] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetBuffer);
    gSwapChainVtable[10] = reinterpret_cast<uint64_t>(&mock_SwapChain_SetFullscreenState);
    gSwapChainVtable[11] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetFullscreenState);
    gSwapChainVtable[12] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetDesc);
    gSwapChainVtable[13] = reinterpret_cast<uint64_t>(&mock_SwapChain_ResizeBuffers);
    gSwapChainVtable[14] = reinterpret_cast<uint64_t>(&mock_SwapChain_ResizeTarget);
    gSwapChainVtable[15] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetContainingOutput);
    gSwapChainVtable[16] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetFrameStatistics);
    gSwapChainVtable[17] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetLastPresentCount);
    gSwapChainVtable[18] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetDesc1);
    gSwapChainVtable[19] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetFullscreenDesc);
    gSwapChainVtable[20] = reinterpret_cast<uint64_t>(&mock_SwapChain_GetHwnd);
    gSwapChainVtable[22] = reinterpret_cast<uint64_t>(&mock_SwapChain_Present1);

    // 8. ID3D11Texture2D & ID3D11View slots
    gTexture2DVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);
    gTexture2DVtable[7] = reinterpret_cast<uint64_t>(&mock_Resource_GetType);
    gTexture2DVtable[10] = reinterpret_cast<uint64_t>(&mock_Texture2D_GetDesc);

    gViewVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);
    gViewVtable[7] = reinterpret_cast<uint64_t>(&mock_View_GetResource);

    // 9. ID3D11GenericResource slots
    gGenericResourceVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);

    // 10. ID3D11Device slots
    gDeviceVtable[3] = reinterpret_cast<uint64_t>(&mock_Device_CreateBuffer);
    gDeviceVtable[5] = reinterpret_cast<uint64_t>(&mock_Device_CreateTexture2D);
    gDeviceVtable[7] = reinterpret_cast<uint64_t>(&mock_Device_CreateShaderResourceView);
    gDeviceVtable[9] = reinterpret_cast<uint64_t>(&mock_Device_CreateRenderTargetView); // CreateRenderTargetView
    gDeviceVtable[10] = reinterpret_cast<uint64_t>(&mock_Device_CreateDepthStencilView); // CreateDepthStencilView
    gDeviceVtable[11] = reinterpret_cast<uint64_t>(&mock_Device_CreateInputLayout);
    gDeviceVtable[12] = reinterpret_cast<uint64_t>(&mock_Device_CreateVertexShader); // CreateVertexShader
    gDeviceVtable[15] = reinterpret_cast<uint64_t>(&mock_Device_CreatePixelShader);  // CreatePixelShader
    gDeviceVtable[20] = reinterpret_cast<uint64_t>(&mock_Device_CreateBlendState);
    gDeviceVtable[21] = reinterpret_cast<uint64_t>(&mock_Device_CreateDepthStencilState);
    gDeviceVtable[22] = reinterpret_cast<uint64_t>(&mock_Device_CreateRasterizerState); // CreateRasterizerState
    gDeviceVtable[23] = reinterpret_cast<uint64_t>(&mock_Device_CreateState);  // CreateSamplerState
    gDeviceVtable[37] = reinterpret_cast<uint64_t>(&mock_Device_GetFeatureLevel);
    gDeviceVtable[39] = reinterpret_cast<uint64_t>(&mock_Device_GetDeviceRemovedReason);
    gDeviceVtable[40] = reinterpret_cast<uint64_t>(&mock_Device_GetImmediateContext);

    // 11. ID3D11DeviceContext slots
    gContextVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);
    gContextVtable[7] = reinterpret_cast<uint64_t>(&mock_Context_VSSetConstantBuffers);
    gContextVtable[8] = reinterpret_cast<uint64_t>(&mock_Context_PSSetShaderResources);
    gContextVtable[9] = reinterpret_cast<uint64_t>(&mock_Context_PSSetShader);
    gContextVtable[10] = reinterpret_cast<uint64_t>(&mock_Context_PSSetSamplers);
    gContextVtable[11] = reinterpret_cast<uint64_t>(&mock_Context_VSSetShader);
    gContextVtable[12] = reinterpret_cast<uint64_t>(&mock_Context_DrawIndexed);
    gContextVtable[13] = reinterpret_cast<uint64_t>(&mock_Context_Draw);
    gContextVtable[14] = reinterpret_cast<uint64_t>(&mock_Context_Map);
    gContextVtable[15] = reinterpret_cast<uint64_t>(&mock_Context_Unmap);
    gContextVtable[16] = reinterpret_cast<uint64_t>(&mock_Context_PSSetConstantBuffers);
    gContextVtable[17] = reinterpret_cast<uint64_t>(&mock_Context_IASetInputLayout);
    gContextVtable[18] = reinterpret_cast<uint64_t>(&mock_Context_IASetVertexBuffers);
    gContextVtable[19] = reinterpret_cast<uint64_t>(&mock_Context_IASetIndexBuffer);
    gContextVtable[24] = reinterpret_cast<uint64_t>(&mock_Context_IASetPrimitiveTopology);
    gContextVtable[25] = reinterpret_cast<uint64_t>(&mock_Context_VSSetShaderResources);
    gContextVtable[26] = reinterpret_cast<uint64_t>(&mock_Context_VSSetSamplers);
    gContextVtable[33] = reinterpret_cast<uint64_t>(&mock_Context_OMSetRenderTargets);
    gContextVtable[35] = reinterpret_cast<uint64_t>(&mock_Context_OMSetBlendState);
    gContextVtable[36] = reinterpret_cast<uint64_t>(&mock_Context_OMSetDepthStencilState);
    gContextVtable[43] = reinterpret_cast<uint64_t>(&mock_Context_RSSetState);
    gContextVtable[44] = reinterpret_cast<uint64_t>(&mock_Context_RSSetViewports);
    gContextVtable[45] = reinterpret_cast<uint64_t>(&mock_Context_RSSetScissorRects);
    gContextVtable[46] = reinterpret_cast<uint64_t>(&mock_Context_CopySubresourceRegion);
    gContextVtable[47] = reinterpret_cast<uint64_t>(&mock_Context_CopyResource);
    gContextVtable[48] = reinterpret_cast<uint64_t>(&mock_Context_UpdateSubresource);
    gContextVtable[50] = reinterpret_cast<uint64_t>(&mock_Context_ClearRenderTargetView);
    gContextVtable[53] = reinterpret_cast<uint64_t>(&mock_Context_ClearDepthStencilView);

    // 12. ID3D11ShaderReflection slots
    gShaderReflectionVtable[3] = reinterpret_cast<uint64_t>(&mock_Reflection_GetDesc);
    gShaderReflectionVtable[4] = reinterpret_cast<uint64_t>(&mock_Reflection_GetConstantBufferByIndex); // GetConstantBufferByIndex
    gShaderReflectionVtable[5] = reinterpret_cast<uint64_t>(&mock_Reflection_GetConstantBufferByName);  // GetConstantBufferByName
    gShaderReflectionVtable[6] = reinterpret_cast<uint64_t>(&mock_Reflection_GetResourceBindingDesc);
    gShaderReflectionVtable[7] = reinterpret_cast<uint64_t>(&mock_Reflection_GetParameterDesc); // GetInputParameterDesc
    gShaderReflectionVtable[8] = reinterpret_cast<uint64_t>(&mock_Reflection_GetParameterDesc); // GetOutputParameterDesc

    // 13. ID3D11ShaderReflectionConstantBuffer slots (bắt đầu từ slot 0)
    gShaderReflectionConstantBufferVtable[0] = reinterpret_cast<uint64_t>(&mock_ConstantBuffer_GetDesc);
    gShaderReflectionConstantBufferVtable[1] = reinterpret_cast<uint64_t>(&mock_ConstantBuffer_GetVariableByIndex);
    gShaderReflectionConstantBufferVtable[2] = reinterpret_cast<uint64_t>(&mock_ConstantBuffer_GetVariableByName);

    // 14. ID3D11ShaderReflectionVariable slots (bắt đầu từ slot 0)
    gShaderReflectionVariableVtable[0] = reinterpret_cast<uint64_t>(&mock_Variable_GetDesc);
    gShaderReflectionVariableVtable[1] = reinterpret_cast<uint64_t>(&mock_Variable_GetType);
    gShaderReflectionVariableVtable[2] = reinterpret_cast<uint64_t>(&mock_Variable_GetConstantBuffer);

    // 15. ID3D11ShaderReflectionType slots (bắt đầu từ slot 0)
    gShaderReflectionTypeVtable[0] = reinterpret_cast<uint64_t>(&mock_Type_GetDesc);
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

int32_t WINAPI D3DReflect(
    const void* pSrcData,
    size_t srcDataSize,
    const void* pInterface,
    void** ppReflector
) {
    initVtablesOnce();
    if (!sgre::parseDxbcRdef(pSrcData, srcDataSize, gCurrentReflectionData)) {
        gCurrentReflectionData = sgre::DxbcReflectionData{};
        LOGW("D3DReflect: Không đọc được RDEF từ bytecode (size=%zu), dùng cấu trúc rỗng an toàn", srcDataSize);
    }
    syncMockVariablesFromCurrentData();

    static uint32_t sReflectLogCount = 0;
    if (++sReflectLogCount <= 30 || sReflectLogCount % 500 == 1) {
        size_t cbCount = gCurrentReflectionData.constantBuffers.size();
        size_t varCount = cbCount > 0 ? gCurrentReflectionData.constantBuffers[0].variables.size() : 0;
        uint32_t cbSize = cbCount > 0 ? gCurrentReflectionData.constantBuffers[0].size : 0;
        LOGI("D3DReflect #%u: Size=%zu, CBs=%zu (cb0Size=%u, vars=%zu), RBs=%zu",
             sReflectLogCount, srcDataSize, cbCount, cbSize, varCount, gCurrentReflectionData.boundResources.size());
    }

    if (ppReflector) {
        *ppReflector = &gMockShaderReflection;
    }
    return 0; // S_OK
}
}
