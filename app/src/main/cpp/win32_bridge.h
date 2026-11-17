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

// Cập nhật trạng thái Virtual-Key (VK_*) của bàn phím / chuột
void Win32Bridge_SetKeyState(uint8_t vkCode, bool isDown);

// Truy vấn trạng thái Virtual-Key (VK_*)
bool Win32Bridge_GetKeyState(uint8_t vkCode);

// Cập nhật toạ độ con trỏ chuột
void Win32Bridge_SetCursorPos(int32_t x, int32_t y);

// Thiết lập thông tin TLS của binary PE để hỗ trợ đa luồng
void Win32Bridge_SetTlsInfo(uint32_t tlsRva, uint64_t origImageBase);

// Khởi tạo Windows TEB & cấu hình thanh ghi GS cho luồng hiện tại
bool Win32Bridge_SetupCurrentThreadTEB();

// Điều tiết Virtual Clock khi một khung hình D3D11 được Present
void Win32Bridge_OnFramePresented();

// Đánh thức hoặc báo hiệu Windows Event Handle từ bất kỳ luồng nào
int32_t Win32Bridge_SetEvent(void* hEvent);

#ifdef __cplusplus
}
#endif
