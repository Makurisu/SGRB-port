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

// Khởi tạo bảng Win32 Stub Table & Universal Function Resolver
void Win32Bridge_Init();

// Hàm trả về con trỏ hàm Win32 API stub dựa theo tên DLL và tên hàm
void* Win32Bridge_ResolveSymbol(const char* dllName, const char* funcName);

#ifdef __cplusplus
}
#endif
