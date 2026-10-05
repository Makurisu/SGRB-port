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

bool D3D11Bridge_WaitForNextFrame(RenderFrameState* outState, uint32_t timeoutMs);
void D3D11Bridge_GetLatestFrameState(RenderFrameState* outState);

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
