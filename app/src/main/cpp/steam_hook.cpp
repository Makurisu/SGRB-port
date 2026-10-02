#include <cstdint>
#include <android/log.h>

#define LOG_TAG "SGRE_SteamHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Giả lập chuẩn 9 hàm Steamworks được import từ IAT gốc của sgre_steam.exe
extern "C" {

int32_t SteamAPI_GetHSteamUser() {
    LOGI("SteamAPI_GetHSteamUser called -> Mock User ID 1");
    return 1;
}

void SteamAPI_Shutdown() {
    LOGI("SteamAPI_Shutdown called");
}

bool SteamAPI_RestartAppIfNecessary(uint32_t /* unOwnAppID */) {
    LOGI("SteamAPI_RestartAppIfNecessary called -> False (Không cần restart qua Steam)");
    return false;
}

void SteamAPI_RunCallbacks() {
    // Giữ callbacks loop hoạt động bình thường
}

bool SteamInternal_SteamAPI_Init(void* /* pOutErrMsg */, int /* nOutErrMsgSize */) {
    LOGI("SteamInternal_SteamAPI_Init called -> Mock Success");
    return true;
}

void* SteamInternal_FindOrCreateUserInterface(int32_t /* hSteamUser */, const char* pszVersion) {
    LOGI("SteamInternal_FindOrCreateUserInterface called for: %s", pszVersion ? pszVersion : "Unknown");
    static uint64_t dummyVtable[16] = {0};
    return dummyVtable;
}

void SteamAPI_RegisterCallback(void* /* pCallback */, int /* iCallback */) {
    // Đăng ký callback giả lập
}

void SteamAPI_UnregisterCallback(void* /* pCallback */) {
    // Hủy callback giả lập
}

void SteamInternal_ContextInit(void* /* pContext */) {
    LOGI("SteamInternal_ContextInit called");
}

}
