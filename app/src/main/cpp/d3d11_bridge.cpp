#include "d3d11_bridge.h"
#include "resource_registry.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>

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

// Vùng nhớ đệm an toàn cho ID3D11DeviceContext::Map
static uint8_t sMappedScratchMemory[16 * 1024 * 1024]; // 16 MB

// Quản lý tài nguyên CPU
static sgre::ResourceRegistry sRegistry(sgre::kDefaultCpuBudgetBytes);

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

    // Mặc định trả về chính con trỏ đối tượng hiện tại
    *ppvObject = thisPtr;
    return 0; // S_OK
}

static uint32_t WINAPI mock_COM_AddRef(void* thisPtr) {
    int32_t ref = sRegistry.addRef(thisPtr);
    return ref > 0 ? static_cast<uint32_t>(ref) : 1;
}
static std::mutex sDeferredMutex;
static std::vector<void*> sDeferredReleaseList;

static uint32_t WINAPI mock_COM_Release(void* thisPtr) {
    int32_t ref = sRegistry.release(thisPtr);
    if (ref == 0) {
        // Trì hoãn giải phóng heap pointer cho đến Present để tránh tái cấp phát đè ô nhớ trong cùng frame
        if (thisPtr != &gMockDevice &&
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
static std::atomic<uint32_t> sTotalDrawIndexedCalls{0};
static std::atomic<uint32_t> sTotalDrawCalls{0};
static RenderFrameState sCurrentFrameState = { {0.0f, 0.0f, 0.0f, 1.0f}, 0, 1920, 1080, 0, 0, 0, 0, 0, 0, 0 };
static bool sNewFrameAvailable = false;

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
static const void* sCurrentVertexBuffer = nullptr;
static uint32_t sCurrentVertexStride = 20; // 5 floats/fields = 20 bytes: pos.x, pos.y, color, uv.u, uv.v
static uint32_t sCurrentVertexOffset = 0;
static const void* sCurrentIndexBuffer = nullptr;
static uint32_t sCurrentIndexFormat = 57; // DXGI_FORMAT_R16_UINT = 57
static const void* sActiveShaderResourceView = nullptr;
static const void* sActiveTextureResource = nullptr;

// IDXGISwapChain methods
static int64_t WINAPI mock_SwapChain_GetDevice(void*, const void*, void** ppDevice) {
    if (ppDevice) *ppDevice = &gMockDevice;
    return 0;
}

static int64_t WINAPI mock_SwapChain_Present(void*, uint32_t syncInterval, uint32_t flags) {
    static uint32_t frameCount = 0;
    frameCount++;
    if (frameCount % 60 == 1) {
        LOGI("IDXGISwapChain::Present: frame %u, syncInterval=%u, flags=0x%x", frameCount, syncInterval, flags);
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
    }
    {
        std::lock_guard<std::mutex> sLock(sSpriteMutex);
        sReadyFrameSprites = std::move(sActiveDrawingSprites);
        sActiveDrawingSprites.clear();
    }
    {
        std::lock_guard<std::mutex> lock(sDeferredMutex);
        for (void* ptr : sDeferredReleaseList) {
            delete reinterpret_cast<SGRE_COM_Mock*>(ptr);
        }
        sDeferredReleaseList.clear();
    }
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
    static uint32_t frameCount1 = 0;
    frameCount1++;
    if (frameCount1 % 60 == 1) {
        LOGI("IDXGISwapChain1::Present1: frame %u, syncInterval=%u, flags=0x%x", frameCount1, syncInterval, flags);
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
    }
    {
        std::lock_guard<std::mutex> sLock(sSpriteMutex);
        sReadyFrameSprites = std::move(sActiveDrawingSprites);
        sActiveDrawingSprites.clear();
    }
    {
        std::lock_guard<std::mutex> lock(sDeferredMutex);
        for (void* ptr : sDeferredReleaseList) {
            delete reinterpret_cast<SGRE_COM_Mock*>(ptr);
        }
        sDeferredReleaseList.clear();
    }
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
static void WINAPI mock_Texture2D_GetDesc(void*, void* pDesc) {
    if (pDesc) {
        auto* desc = reinterpret_cast<MockTexture2DDesc*>(pDesc);
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

// ID3D11Device methods
static int64_t WINAPI mock_Device_CreateBuffer(void*, const void* pDesc, const void* pInitialData, void** ppBuffer) {
    sTotalBuffersCreated++;
    auto* mockBuf = new SGRE_COM_Mock{ gGenericResourceVtable };
    if (pDesc) {
        const auto* desc = reinterpret_cast<const MockBufferDesc*>(pDesc);
        LOGI("ID3D11Device::CreateBuffer: mockBuf=%p, ByteWidth=%u, Usage=%u, BindFlags=0x%x, InitialData=%p",
             mockBuf, desc->byteWidth, desc->usage, desc->bindFlags, pInitialData);

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
                if (++sBuf80Count % 60 == 1) {
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
    sTotalTexturesCreated++;
    auto* mockTex = new SGRE_COM_Mock{ gTexture2DVtable };
    if (pDesc) {
        const auto* desc = reinterpret_cast<const MockTexture2DDesc*>(pDesc);
        LOGI("ID3D11Device::CreateTexture2D: %ux%u, Mips=%u, Array=%u, Format=%u, BindFlags=0x%x, InitialData=%p",
             desc->width, desc->height, desc->mipLevels, desc->arraySize, desc->format, desc->bindFlags, pInitialData);

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

        const void* initMem = nullptr;
        uint32_t initPitch = 0;
        if (pInitialData) {
            const auto* subData = reinterpret_cast<const MockSubresourceData*>(pInitialData);
            initMem = subData->pSysMem;
            initPitch = subData->sysMemPitch;
        }
        sRegistry.registerTexture(mockTex, texDesc, initMem, initPitch);
    }
    if (ppTexture2D) *ppTexture2D = mockTex;
    return 0;
}

struct MockViewCOM : public SGRE_COM_Mock {
    void* pResource;
};

static int64_t WINAPI mock_Device_CreateShaderResourceView(void*, void* pResource, const void*, void** ppView) {
    if (ppView) {
        auto* view = new MockViewCOM();
        view->vtable = gGenericResourceVtable;
        view->pResource = pResource;
        *ppView = view;
    }
    return 0;
}

static int64_t WINAPI mock_Device_CreateView(void*, void*, const void*, void** ppView) {
    if (ppView) *ppView = &gMockGenericResource;
    return 0;
}

static int64_t WINAPI mock_Device_CreateShader(void*, const void* pBytecode, size_t bytecodeLength, void*, void** ppShader) {
    sTotalShadersCreated++;
    LOGI("ID3D11Device::CreateShader: BytecodeLength=%zu, Bytecode=%p", bytecodeLength, pBytecode);
    if (ppShader) *ppShader = &gMockGenericResource;
    return 0;
}

static int64_t WINAPI mock_Device_CreateInputLayout(void*, const void*, uint32_t, const void*, size_t, void** ppLayout) {
    if (ppLayout) *ppLayout = &gMockGenericResource;
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

// ID3D11DeviceContext methods
static void WINAPI mock_Context_DrawIndexed(void*, uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) {
    sTotalDrawIndexedCalls++;
    static uint32_t sDrawIndexedCount = 0;
    if (++sDrawIndexedCount % 60 == 1) {
        LOGI("ID3D11DeviceContext::DrawIndexed: IndexCount=%u, StartIndex=%u, BaseVertex=%d (total calls: %u)",
             indexCount, startIndexLocation, baseVertexLocation, sDrawIndexedCount);
    }

    if (sCurrentVertexBuffer != nullptr && indexCount >= 4) {
        sgre::MappedView vView{};
        sgre::BufferDesc vDesc{};
        if (sRegistry.getBufferDesc(sCurrentVertexBuffer, vDesc) && sRegistry.map(sCurrentVertexBuffer, vView) && vView.data != nullptr) {
            sgre::MappedView iView{};
            const uint16_t* idx16 = nullptr;
            const uint32_t* idx32 = nullptr;
            if (sCurrentIndexBuffer != nullptr && sRegistry.map(sCurrentIndexBuffer, iView) && iView.data != nullptr) {
                if (sCurrentIndexFormat == 57) { // DXGI_FORMAT_R16_UINT
                    idx16 = reinterpret_cast<const uint16_t*>(iView.data);
                } else if (sCurrentIndexFormat == 42) { // DXGI_FORMAT_R32_UINT
                    idx32 = reinterpret_cast<const uint32_t*>(iView.data);
                }
            }

            SpriteDrawCommand cmd{};
            bool validQuad = true;

            for (uint32_t i = 0; i < 4; ++i) {
                int32_t vertexIdx = static_cast<int32_t>(i);
                if (idx16 != nullptr) {
                    vertexIdx = static_cast<int32_t>(idx16[startIndexLocation + i]);
                } else if (idx32 != nullptr) {
                    vertexIdx = static_cast<int32_t>(idx32[startIndexLocation + i]);
                }
                vertexIdx += baseVertexLocation;

                size_t byteOffset = sCurrentVertexOffset + static_cast<size_t>(vertexIdx) * sCurrentVertexStride;
                if (byteOffset + sCurrentVertexStride > vDesc.byteWidth) {
                    validQuad = false;
                    break;
                }

                const uint8_t* vertPtr = vView.data + byteOffset;
                float rawX = *reinterpret_cast<const float*>(vertPtr + 0);
                float rawY = *reinterpret_cast<const float*>(vertPtr + 4);
                float rawU = *reinterpret_cast<const float*>(vertPtr + 12);
                float rawV = *reinterpret_cast<const float*>(vertPtr + 16);

                if (std::abs(rawX) > 1.5f || std::abs(rawY) > 1.5f) {
                    cmd.pos[i][0] = rawX / 960.0f;
                    cmd.pos[i][1] = -rawY / 540.0f;
                } else {
                    cmd.pos[i][0] = rawX;
                    cmd.pos[i][1] = rawY;
                }

                if (std::isnan(rawU) || std::isinf(rawU) || rawU < -10.0f || rawU > 10.0f) rawU = 0.0f;
                if (std::isnan(rawV) || std::isinf(rawV) || rawV < -10.0f || rawV > 10.0f) rawV = 0.0f;
                cmd.uv[i][0] = rawU;
                cmd.uv[i][1] = rawV;
            }

            static uint32_t sDiagCount = 0;
            if (++sDiagCount % 120 == 1) {
                float dbgX = (vDesc.byteWidth >= 8) ? *reinterpret_cast<const float*>(vView.data) : 0.0f;
                float dbgY = (vDesc.byteWidth >= 8) ? *reinterpret_cast<const float*>(vView.data + 4) : 0.0f;
                LOGI("DrawIndexed Diag: VbWidth=%u, Offset=%u, valid=%d, degen=%d, rawX0=%.1f, rawY0=%.1f",
                     vDesc.byteWidth, sCurrentVertexOffset, validQuad, isDegenerate, dbgX, dbgY);
            }

            if (validQuad && !isDegenerate) {
                    cmd.indices[0] = 0; cmd.indices[1] = 1; cmd.indices[2] = 2;
                    cmd.indices[3] = 2; cmd.indices[4] = 1; cmd.indices[5] = 3;
                    cmd.textureHandle = sActiveTextureResource;
                    cmd.textureWidth = 0;
                    cmd.textureHeight = 0;
                    if (sActiveTextureResource != nullptr) {
                        sgre::TextureDesc tDesc{};
                        if (sRegistry.getTextureDesc(sActiveTextureResource, tDesc)) {
                            cmd.textureWidth = tDesc.width;
                            cmd.textureHeight = tDesc.height;
                        }
                    }

                    static uint32_t sSpriteLogCount = 0;
                    if (++sSpriteLogCount % 60 == 1) {
                        LOGI("Extracted Valid Sprite #%u: Tex=%p (%ux%u) | NDC: V0=(%.2f, %.2f) V1=(%.2f, %.2f) V2=(%.2f, %.2f) V3=(%.2f, %.2f) | UV0=[%.2f, %.2f]",
                             sSpriteLogCount,
                             cmd.textureHandle, cmd.textureWidth, cmd.textureHeight,
                             cmd.pos[0][0], cmd.pos[0][1], cmd.pos[1][0], cmd.pos[1][1],
                             cmd.pos[2][0], cmd.pos[2][1], cmd.pos[3][0], cmd.pos[3][1],
                             cmd.uv[0][0], cmd.uv[0][1]);
                    }

                    std::lock_guard<std::mutex> lock(sSpriteMutex);
                    if (sActiveDrawingSprites.size() < 1024) {
                        sActiveDrawingSprites.push_back(cmd);
                    }
                }
            }
        }
    }
}

static void WINAPI mock_Context_Draw(void*, uint32_t vertexCount, uint32_t startVertexLocation) {
    sTotalDrawCalls++;
    static uint32_t sDrawCount = 0;
    if (++sDrawCount % 60 == 1) {
        LOGI("ID3D11DeviceContext::Draw: VertexCount=%u, StartVertex=%u (total calls: %u)",
             vertexCount, startVertexLocation, sDrawCount);
    }
}

static int64_t WINAPI mock_Context_Map(void*, void* pResource, uint32_t subresource, uint32_t mapType, uint32_t mapFlags, void* pMappedResource) {
    static uint32_t sMapCount = 0;
    if (++sMapCount % 60 == 1) {
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
    if (++sUnmapCount % 60 == 1) {
        LOGI("ID3D11DeviceContext::Unmap: Resource=%p, Subresource=%u", pResource, subresource);
    }
}

static void WINAPI mock_Context_RSSetViewports(void*, uint32_t numViewports, const MockViewport* pViewports) {
    if (numViewports > 0 && pViewports) {
        static uint32_t sVpCount = 0;
        if (++sVpCount % 60 == 1) {
            LOGI("ID3D11DeviceContext::RSSetViewports: [%.0f, %.0f, %.0fx%.0f, depth: %.1f-%.1f]",
                 pViewports[0].topLeftX, pViewports[0].topLeftY,
                 pViewports[0].width, pViewports[0].height,
                 pViewports[0].minDepth, pViewports[0].maxDepth);
        }
    }
}

static void WINAPI mock_Context_RSSetScissorRects(void*, uint32_t numRects, const MockRect* pRects) {
    if (numRects > 0 && pRects) {
        static uint32_t sScissorCount = 0;
        if (++sScissorCount % 60 == 1) {
            LOGI("ID3D11DeviceContext::RSSetScissorRects: [%d, %d, %d, %d]",
                 pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom);
        }
    }
}

static void WINAPI mock_Context_UpdateSubresource(
    void*, void* pDstResource, uint32_t dstSubresource, const void* pDstBox,
    const void* pSrcData, uint32_t srcRowPitch, uint32_t srcDepthPitch
) {
    static uint32_t sUpdateCount = 0;
    if (++sUpdateCount % 60 == 1) {
        LOGI("ID3D11DeviceContext::UpdateSubresource: Dst=%p, Subres=%u, Src=%p, RowPitch=%u, DepthPitch=%u",
             pDstResource, dstSubresource, pSrcData, srcRowPitch, srcDepthPitch);
    }
    if (pDstResource && pSrcData) {
        const auto* box = reinterpret_cast<const sgre::UpdateBox*>(pDstBox);
        if (!sRegistry.updateBuffer(pDstResource, box, pSrcData)) {
            sRegistry.updateTexture(pDstResource, box, pSrcData, srcRowPitch);
        }
    }
}

static void WINAPI mock_Context_VSSetConstantBuffers(void*, uint32_t, uint32_t, void* const*) {}
static void WINAPI mock_Context_PSSetConstantBuffers(void*, uint32_t, uint32_t, void* const*) {}

static void WINAPI mock_Context_PSSetShaderResources(void*, uint32_t startSlot, uint32_t numViews, void* const* ppShaderResourceViews) {
    if (numViews > 0 && ppShaderResourceViews && ppShaderResourceViews[0]) {
        sActiveShaderResourceView = ppShaderResourceViews[0];
        // Trích xuất con trỏ ID3D11Texture2D gốc từ MockViewCOM nếu có
        auto* view = reinterpret_cast<const MockViewCOM*>(ppShaderResourceViews[0]);
        if (view->pResource != nullptr) {
            sActiveTextureResource = view->pResource;
        } else {
            sActiveTextureResource = ppShaderResourceViews[0];
        }
    }
}

static void WINAPI mock_Context_PSSetShader(void*, void*, void* const*, uint32_t) {}
static void WINAPI mock_Context_PSSetSamplers(void*, uint32_t, uint32_t, void* const*) {}
static void WINAPI mock_Context_VSSetShader(void*, void*, void* const*, uint32_t) {}

static void WINAPI mock_Context_IASetVertexBuffers(void*, uint32_t startSlot, uint32_t numBuffers, void* const* ppVertexBuffers, const uint32_t* pStrides, const uint32_t* pOffsets) {
    static uint32_t sVbLogCount = 0;
    if (numBuffers > 0 && ppVertexBuffers && ppVertexBuffers[0]) {
        void* newVb = ppVertexBuffers[0];
        if (sCurrentVertexBuffer != newVb) {
            if (sCurrentVertexBuffer != nullptr) {
                sRegistry.release(sCurrentVertexBuffer);
            }
            sCurrentVertexBuffer = newVb;
            sRegistry.addRef(sCurrentVertexBuffer);
        }
        if (pStrides && pStrides[0] > 0) {
            sCurrentVertexStride = pStrides[0];
        }
        sCurrentVertexOffset = (pOffsets && pOffsets[0] > 0) ? pOffsets[0] : 0;
        if (++sVbLogCount % 60 == 1) {
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

static void WINAPI mock_Context_IASetIndexBuffer(void*, void* pIndexBuffer, uint32_t format, uint32_t) {
    if (pIndexBuffer) {
        if (sCurrentIndexBuffer != pIndexBuffer) {
            if (sCurrentIndexBuffer != nullptr) {
                sRegistry.release(sCurrentIndexBuffer);
            }
            sCurrentIndexBuffer = pIndexBuffer;
            sRegistry.addRef(sCurrentIndexBuffer);
        }
        sCurrentIndexFormat = format;
        static uint32_t sIbLogCount = 0;
        if (++sIbLogCount % 60 == 1) {
            sgre::BufferDesc bDesc{};
            uint32_t bWidth = 0;
            if (sRegistry.getBufferDesc(sCurrentIndexBuffer, bDesc)) {
                bWidth = bDesc.byteWidth;
            }
            LOGI("IASetIndexBuffer: Ib=%p (Width=%u), Format=%u", sCurrentIndexBuffer, bWidth, format);
        }
    }
}

static void WINAPI mock_Context_IASetInputLayout(void*, void*) {}
static void WINAPI mock_Context_IASetPrimitiveTopology(void*, uint32_t) {}
static void WINAPI mock_Context_VSSetShaderResources(void*, uint32_t, uint32_t, void* const*) {}
static void WINAPI mock_Context_VSSetSamplers(void*, uint32_t, uint32_t, void* const*) {}
static void WINAPI mock_Context_OMSetRenderTargets(void*, uint32_t, void* const*, void*) {}
static void WINAPI mock_Context_OMSetBlendState(void*, void*, const float[4], uint32_t) {}
static void WINAPI mock_Context_OMSetDepthStencilState(void*, void*, uint32_t) {}
static void WINAPI mock_Context_RSSetState(void*, void*) {}
static void WINAPI mock_Context_ClearDepthStencilView(void*, void*, uint32_t, float, uint8_t) {}

static void WINAPI mock_Context_ClearRenderTargetView(void*, void*, const float colorRGBA[4]) {
    if (colorRGBA) {
        std::lock_guard<std::mutex> lock(sFrameMutex);
        sCurrentFrameState.clearColor[0] = colorRGBA[0];
        sCurrentFrameState.clearColor[1] = colorRGBA[1];
        sCurrentFrameState.clearColor[2] = colorRGBA[2];
        sCurrentFrameState.clearColor[3] = colorRGBA[3];
        static uint32_t clearCount = 0;
        if (++clearCount % 60 == 1) {
            LOGI("ClearRenderTargetView: RGBA=(%.3f, %.3f, %.3f, %.3f)",
                 colorRGBA[0], colorRGBA[1], colorRGBA[2], colorRGBA[3]);
        }
    }
}

bool D3D11Bridge_GetActiveTextureInfo(uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData) {
    return D3D11Bridge_GetTextureInfoByHandle(sActiveTextureResource, outWidth, outHeight, outRgbaData);
}

bool D3D11Bridge_GetTextureInfoByHandle(const void* handle, uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData) {
    if (handle == nullptr) return false;

    sgre::TextureDesc desc{};
    if (!sRegistry.getTextureDesc(handle, desc)) return false;

    sgre::MappedView view{};
    if (!sRegistry.map(handle, view)) return false;

    if (outWidth) *outWidth = desc.width;
    if (outHeight) *outHeight = desc.height;
    if (outRgbaData) *outRgbaData = view.data;
    return true;
}

size_t D3D11Bridge_GetSpriteBatchInfo(SpriteDrawCommand* outCommands, size_t maxCommands) {
    std::lock_guard<std::mutex> lock(sSpriteMutex);
    size_t count = std::min(sReadyFrameSprites.size(), maxCommands);
    if (outCommands != nullptr && count > 0) {
        std::memcpy(outCommands, sReadyFrameSprites.data(), count * sizeof(SpriteDrawCommand));
    }
    return count;
}

bool D3D11Bridge_WaitForNextFrame(RenderFrameState* outState, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(sFrameMutex);
    if (!sNewFrameAvailable) {
        if (timeoutMs == 0) return false;
        bool signaled = sFrameCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), []{
            return sNewFrameAvailable;
        });
        if (!signaled) return false;
    }
    sNewFrameAvailable = false;
    if (outState) {
        *outState = sCurrentFrameState;
    }
    return true;
}

void D3D11Bridge_GetLatestFrameState(RenderFrameState* outState) {
    std::lock_guard<std::mutex> lock(sFrameMutex);
    if (outState) {
        *outState = sCurrentFrameState;
    }
}

// ID3D11ShaderReflection methods
static int64_t WINAPI mock_Reflection_GetDesc(void*, void* pDesc) {
    LOGI("ID3D11ShaderReflection::GetDesc called");
    if (pDesc) {
        memset(pDesc, 0, 256);
        auto* desc32 = reinterpret_cast<uint32_t*>(pDesc);
        desc32[0] = 0x500; // D3D11 shader model 5.0
    }
    return 0; // S_OK
}

static void* WINAPI mock_Reflection_GetConstantBuffer(void*, uint32_t) {
    return &gMockShaderConstantBuffer;
}

static int64_t WINAPI mock_Reflection_GetResourceBindingDesc(void*, uint32_t, void* pDesc) {
    if (pDesc) memset(pDesc, 0, 128);
    return 0; // S_OK
}

static int64_t WINAPI mock_Reflection_GetParameterDesc(void*, uint32_t, void* pDesc) {
    if (pDesc) memset(pDesc, 0, 128);
    return 0; // S_OK
}

static void* WINAPI mock_Reflection_GetVariable(void*, const char*) {
    return &gMockShaderVariable;
}

static void* WINAPI mock_Reflection_GetType(void*) {
    return &gMockShaderType;
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
        gSwapChainVtable, gTexture2DVtable, gGenericResourceVtable,
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

    // 8. ID3D11Texture2D slots
    gTexture2DVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);
    gTexture2DVtable[7] = reinterpret_cast<uint64_t>(&mock_Resource_GetType);
    gTexture2DVtable[10] = reinterpret_cast<uint64_t>(&mock_Texture2D_GetDesc);

    // 9. ID3D11GenericResource slots
    gGenericResourceVtable[3] = reinterpret_cast<uint64_t>(&mock_Resource_GetDevice);

    // 10. ID3D11Device slots
    gDeviceVtable[3] = reinterpret_cast<uint64_t>(&mock_Device_CreateBuffer);
    gDeviceVtable[5] = reinterpret_cast<uint64_t>(&mock_Device_CreateTexture2D);
    gDeviceVtable[7] = reinterpret_cast<uint64_t>(&mock_Device_CreateShaderResourceView);
    gDeviceVtable[9] = reinterpret_cast<uint64_t>(&mock_Device_CreateView); // CreateRenderTargetView
    gDeviceVtable[10] = reinterpret_cast<uint64_t>(&mock_Device_CreateView); // CreateDepthStencilView
    gDeviceVtable[11] = reinterpret_cast<uint64_t>(&mock_Device_CreateInputLayout);
    gDeviceVtable[12] = reinterpret_cast<uint64_t>(&mock_Device_CreateShader); // CreateVertexShader
    gDeviceVtable[15] = reinterpret_cast<uint64_t>(&mock_Device_CreateShader); // CreatePixelShader
    gDeviceVtable[20] = reinterpret_cast<uint64_t>(&mock_Device_CreateState);  // CreateBlendState
    gDeviceVtable[21] = reinterpret_cast<uint64_t>(&mock_Device_CreateState);  // CreateDepthStencilState
    gDeviceVtable[22] = reinterpret_cast<uint64_t>(&mock_Device_CreateState);  // CreateRasterizerState
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
    gContextVtable[48] = reinterpret_cast<uint64_t>(&mock_Context_UpdateSubresource);
    gContextVtable[50] = reinterpret_cast<uint64_t>(&mock_Context_ClearRenderTargetView);
    gContextVtable[53] = reinterpret_cast<uint64_t>(&mock_Context_ClearDepthStencilView);

    // 12. ID3D11ShaderReflection slots
    gShaderReflectionVtable[3] = reinterpret_cast<uint64_t>(&mock_Reflection_GetDesc);
    gShaderReflectionVtable[4] = reinterpret_cast<uint64_t>(&mock_Reflection_GetConstantBuffer); // GetConstantBufferByIndex
    gShaderReflectionVtable[5] = reinterpret_cast<uint64_t>(&mock_Reflection_GetConstantBuffer); // GetConstantBufferByName
    gShaderReflectionVtable[6] = reinterpret_cast<uint64_t>(&mock_Reflection_GetResourceBindingDesc);
    gShaderReflectionVtable[7] = reinterpret_cast<uint64_t>(&mock_Reflection_GetParameterDesc); // GetInputParameterDesc
    gShaderReflectionVtable[8] = reinterpret_cast<uint64_t>(&mock_Reflection_GetParameterDesc); // GetOutputParameterDesc

    // 13. ID3D11ShaderReflectionConstantBuffer slots
    gShaderReflectionConstantBufferVtable[0] = reinterpret_cast<uint64_t>(&mock_Reflection_GetResourceBindingDesc);
    gShaderReflectionConstantBufferVtable[1] = reinterpret_cast<uint64_t>(&mock_Reflection_GetVariable);
    gShaderReflectionConstantBufferVtable[2] = reinterpret_cast<uint64_t>(&mock_Reflection_GetVariable);

    // 14. ID3D11ShaderReflectionVariable slots
    gShaderReflectionVariableVtable[0] = reinterpret_cast<uint64_t>(&mock_Reflection_GetResourceBindingDesc);
    gShaderReflectionVariableVtable[1] = reinterpret_cast<uint64_t>(&mock_Reflection_GetType);
    gShaderReflectionVariableVtable[2] = reinterpret_cast<uint64_t>(&mock_Reflection_GetConstantBuffer);

    // 15. ID3D11ShaderReflectionType slots
    gShaderReflectionTypeVtable[0] = reinterpret_cast<uint64_t>(&mock_Reflection_GetResourceBindingDesc);
    gShaderReflectionTypeVtable[1] = reinterpret_cast<uint64_t>(&mock_Reflection_GetType);
    gShaderReflectionTypeVtable[2] = reinterpret_cast<uint64_t>(&mock_Reflection_GetType);
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
    const void* /* pSrcData */,
    size_t srcDataSize,
    const void* pInterface,
    void** ppReflector
) {
    initVtablesOnce();
    LOGI("D3DReflect intercepted: SrcDataSize=%zu, pInterface=%p, ppReflector=%p",
         srcDataSize, pInterface, ppReflector);

    if (ppReflector) {
        *ppReflector = &gMockShaderReflection;
        LOGI("-> Đã cấp phát Mock ID3D11ShaderReflection: %p", *ppReflector);
    }
    return 0; // S_OK
}
}
