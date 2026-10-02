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

#ifdef __cplusplus
}
#endif
