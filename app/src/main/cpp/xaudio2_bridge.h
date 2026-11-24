#pragma once
#include <cstdint>
#include "win32_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hàm khởi tạo XAudio2 (tương thích XAudio2 2.7, 2.8, 2.9)
int32_t WINAPI XAudio2Create(void** ppXAudio2, uint32_t Flags, uint32_t XAudio2Processor);

// Tạm dừng hoặc khôi phục luồng trộn âm thanh trung tâm theo vòng đời Activity Android
void XAudio2Bridge_SetLifecyclePaused(bool paused);

#ifdef __cplusplus
}
#endif
