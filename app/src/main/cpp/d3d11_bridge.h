#pragma once
#include <cstdint>

struct SGRE_COM_Mock {
    void* vtable;
};

struct RenderFrameState {
    float clearColor[4];
    uint32_t frameIndex;
    uint32_t width;
    uint32_t height;
    uint32_t drawCalls;
    uint32_t indexDraws;
    uint32_t texturesCreated;
    uint32_t shadersCreated;
    uint32_t buffersCreated;
    uint64_t cpuBytes;
    uint32_t droppedResources;
};

class D3D11Bridge {
public:
    static int32_t createDevice(
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
    );
};

struct SpriteDrawCommand {
    float pos[4][2];      // 4 vertices (x, y) normalized to NDC [-1, 1]
    float uv[4][2];       // 4 vertices (u, v) normalized to [0, 1]
    float color[4][4];    // 4 vertices (r, g, b, a) normalized to [0, 1]
    uint32_t indices[6];  // 2 triangles: [0, 1, 2, 2, 1, 3]
    const void* textureHandle;      // ID3D11Texture2D resource handle (Slot 0 Color)
    uint32_t textureWidth;
    uint32_t textureHeight;
    const void* alphaTextureHandle; // ID3D11Texture2D resource handle (Slot 1 Alpha Mask)
    uint32_t alphaTextureWidth;
    uint32_t alphaTextureHeight;
};

bool D3D11Bridge_WaitForNextFrame(RenderFrameState* outState, uint32_t timeoutMs);
void D3D11Bridge_GetLatestFrameState(RenderFrameState* outState);
bool D3D11Bridge_GetActiveTextureInfo(uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData);
bool D3D11Bridge_GetTextureInfoByHandle(const void* handle, uint32_t* outWidth, uint32_t* outHeight, const uint8_t** outRgbaData, uint32_t* outFormat = nullptr);
size_t D3D11Bridge_GetSpriteBatchInfo(SpriteDrawCommand* outCommands, size_t maxCommands);

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif
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
);
int32_t WINAPI D3DReflect(
    const void* pSrcData,
    size_t srcDataSize,
    const void* pInterface,
    void** ppReflector
);
#ifdef __cplusplus
}
#endif
