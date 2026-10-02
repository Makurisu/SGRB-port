#include <cstdint>
#include <android/log.h>

#define LOG_TAG "SGRE_SteamHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Giả lập các hàm gọi từ steam_api64.dll để game không thoát
extern "C" {

bool SteamAPI_Init() {
    LOGI("SteamAPI_Init called -> Mock Success");
    return true;
}

bool SteamAPI_IsSteamRunning() {
    return true;
}

void SteamAPI_Shutdown() {
    LOGI("SteamAPI_Shutdown called");
}

void SteamAPI_RunCallbacks() {
    // Không làm gì, giữ loop ổn định
}

const char* SteamAPI_GetSteamInstallPath() {
    return "/sdcard";
}

}
