#pragma once
#include <cstdint>

struct SGRE_D3D11_DeviceMock {
    void* vtable;
};

struct SGRE_D3D11_DeviceContextMock {
    void* vtable;
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

#ifdef __cplusplus
extern "C" {
#endif
int32_t D3D11CreateDevice(
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
#ifdef __cplusplus
}
#endif
