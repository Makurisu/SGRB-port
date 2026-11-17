#pragma once
#include <cstdint>

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

int32_t WINAPI DirectInput8Create(
    void* hinst,
    uint32_t dwVersion,
    const void* riidltf,
    void** ppvOut,
    void* punkOuter
);

void DInputBridge_PostTouchAction(int32_t action, int32_t x, int32_t y);
void DInputBridge_PostMouseWheel(int32_t wheelDelta);
void DInputBridge_PostKeyEvent(uint32_t virtualKey, bool isDown);

#ifdef __cplusplus
}
#endif
