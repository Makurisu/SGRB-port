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

// Thiết lập ImageBase thực tế của binary game trong RAM
void Win32Bridge_SetLoadedImageBase(void* imageBase);

// Thiết lập đường dẫn thư mục gốc dữ liệu game trên Android
void Win32Bridge_SetDataDirectory(const char* dataDir);

// Khởi tạo bảng Win32 Stub Table & Universal Function Resolver
void Win32Bridge_Init();

// Hàm trả về con trỏ hàm Win32 API stub dựa theo tên DLL và tên hàm
void* Win32Bridge_ResolveSymbol(const char* dllName, const char* funcName);

// Đẩy thông điệp Win32 vào hàng đợi thông điệp của game
void Win32Bridge_PostMessage(uint32_t message, uint64_t wParam, int64_t lParam);

#ifdef __cplusplus
}
#endif
