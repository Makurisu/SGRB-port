#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Thiết lập thư mục gốc dữ liệu game để đọc cấu hình ngôn ngữ động
void SteamHook_SetDataDirectory(const char* dataDir);

// Ghi đè ngôn ngữ game trực tiếp từ giao diện cài đặt Android (ví dụ: "english", "japanese")
void SteamHook_SetLanguageOverride(const char* language);

// Truy vấn ngôn ngữ hiện hành của game
const char* SteamHook_GetCurrentGameLanguage();

#ifdef __cplusplus
}
#endif
