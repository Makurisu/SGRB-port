#include <cstdint>
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <strings.h>

#define LOG_TAG "SGRE_SteamHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#if defined(__x86_64__)
#define WINAPI __attribute__((ms_abi))
#else
#define WINAPI
#endif

namespace {

// Mock SteamID chuẩn 64-bit (Steam Community ID hợp lệ)
static uint64_t gMockSteamID = 76561197960287930ULL;

// Stub đa năng cho mọi phương thức Steam Interface
// Trả về con trỏ tới gMockSteamID để hỗ trợ các hàm trả về struct/con trỏ như GetSteamID()
static void* WINAPI stub_SteamGeneric() {
    return &gMockSteamID;
}

// Bảng VTABLE chung cho mọi Steam Interface (ISteamUser, ISteamFriends...)
static void* gSteamVtable[64];
static void* gMockSteamInterface = nullptr;

static void initSteamVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 64; ++i) {
        gSteamVtable[i] = reinterpret_cast<void*>(&stub_SteamGeneric);
    }
    gMockSteamInterface = &gSteamVtable[0];
    initialized = true;
}

// ISteamApps implementation
static const char* WINAPI stub_SteamApps_GetCurrentGameLanguage(void* /* thisPtr */) {
    static char sGameLanguage[64] = "english";
    static bool sLoaded = false;
    if (!sLoaded) {
        sLoaded = true;
        const char* iniPaths[] = {
            "/sdcard/sgrb_game_data/configs.user.ini",
            "/sdcard/sgrb_game_data/wind3d11data/steam_settings/configs.user.ini",
            "/sdcard/sgrb_game_data/steam_settings/configs.user.ini",
            "/sdcard/sgrb_game_data/language.txt"
        };
        for (const char* path : iniPaths) {
            FILE* fp = fopen(path, "r");
            if (fp) {
                char line[256];
                while (fgets(line, sizeof(line), fp)) {
                    char* nl = strpbrk(line, "\r\n");
                    if (nl) *nl = '\0';
                    char* eq = strchr(line, '=');
                    if (eq) {
                        *eq = '\0';
                        char* key = line;
                        char* val = eq + 1;
                        while (*key == ' ' || *key == '\t') key++;
                        while (*val == ' ' || *val == '\t') val++;
                        if (strcasecmp(key, "Language") == 0 || strcasecmp(key, "language") == 0) {
                            if (val[0] != '\0') {
                                strncpy(sGameLanguage, val, sizeof(sGameLanguage) - 1);
                                sGameLanguage[sizeof(sGameLanguage) - 1] = '\0';
                                LOGI("ISteamApps::GetCurrentGameLanguage: Doc tu %s -> %s", path, sGameLanguage);
                                break;
                            }
                        }
                    } else if (strstr(path, "language.txt")) {
                        char* val = line;
                        while (*val == ' ' || *val == '\t') val++;
                        if (val[0] != '\0') {
                            strncpy(sGameLanguage, val, sizeof(sGameLanguage) - 1);
                            sGameLanguage[sizeof(sGameLanguage) - 1] = '\0';
                            LOGI("ISteamApps::GetCurrentGameLanguage: Doc tu language.txt -> %s", sGameLanguage);
                            break;
                        }
                    }
                }
                fclose(fp);
                break;
            }
        }
        LOGI("ISteamApps::GetCurrentGameLanguage tra ve: '%s'", sGameLanguage);
    }
    return sGameLanguage;
}

static const char* WINAPI stub_SteamApps_GetAvailableGameLanguages(void* /* thisPtr */) {
    return "english,japanese,schinese,tchinese";
}

static bool WINAPI stub_SteamApps_BIsSubscribedApp(void* /* thisPtr */, uint32_t /* appID */) {
    return true;
}

static bool WINAPI stub_SteamApps_BIsDlcInstalled(void* /* thisPtr */, uint32_t /* appID */) {
    return true;
}

static void* gSteamAppsVtable[64];
static void* gMockSteamAppsInterface = nullptr;

static void initSteamAppsVtable() {
    static bool initialized = false;
    if (initialized) return;
    for (size_t i = 0; i < 64; ++i) {
        gSteamAppsVtable[i] = reinterpret_cast<void*>(&stub_SteamGeneric);
    }
    // Slot 4: GetCurrentGameLanguage
    gSteamAppsVtable[4] = reinterpret_cast<void*>(&stub_SteamApps_GetCurrentGameLanguage);
    // Slot 5: GetAvailableGameLanguages
    gSteamAppsVtable[5] = reinterpret_cast<void*>(&stub_SteamApps_GetAvailableGameLanguages);
    // Slot 6: BIsSubscribedApp
    gSteamAppsVtable[6] = reinterpret_cast<void*>(&stub_SteamApps_BIsSubscribedApp);
    // Slot 7: BIsDlcInstalled
    gSteamAppsVtable[7] = reinterpret_cast<void*>(&stub_SteamApps_BIsDlcInstalled);
    gMockSteamAppsInterface = &gSteamAppsVtable[0];
    initialized = true;
}

} // namespace

// Giả lập chuẩn 9 hàm Steamworks được import từ IAT gốc của sgre_steam.exe
extern "C" {

int32_t WINAPI SteamAPI_GetHSteamUser() {
    LOGI("SteamAPI_GetHSteamUser called -> Mock User ID 1");
    return 1;
}

void WINAPI SteamAPI_Shutdown() {
    LOGI("SteamAPI_Shutdown called");
}

bool WINAPI SteamAPI_RestartAppIfNecessary(uint32_t /* unOwnAppID */) {
    LOGI("SteamAPI_RestartAppIfNecessary called -> False (Khong can restart qua Steam)");
    return false;
}

void WINAPI SteamAPI_RunCallbacks() {
    // Giữ callbacks loop hoạt động bình thường
}

int32_t WINAPI SteamInternal_SteamAPI_Init(void* /* pOutErrMsg */, int /* nOutErrMsgSize */) {
    LOGI("SteamInternal_SteamAPI_Init called -> Mock Success (0 = k_ESteamAPIInitResult_OK)");
    return 0; // 0 = k_ESteamAPIInitResult_OK
}

void* WINAPI SteamInternal_FindOrCreateUserInterface(int32_t /* hSteamUser */, const char* pszVersion) {
    LOGI("SteamInternal_FindOrCreateUserInterface called for: %s", pszVersion ? pszVersion : "Unknown");
    if (pszVersion && (strstr(pszVersion, "STEAMAPPS") || strstr(pszVersion, "SteamApps"))) {
        initSteamAppsVtable();
        return &gMockSteamAppsInterface;
    }
    initSteamVtable();
    return &gMockSteamInterface;
}

void WINAPI SteamAPI_RegisterCallback(void* /* pCallback */, int /* iCallback */) {
    // Đăng ký callback giả lập
}

void WINAPI SteamAPI_UnregisterCallback(void* /* pCallback */) {
    // Hủy callback giả lập
}

void* WINAPI SteamInternal_ContextInit(void* pContextInitData) {
    LOGI("SteamInternal_ContextInit called: %p", pContextInitData);
    if (!pContextInitData) return nullptr;

    struct ContextInitData {
        void (WINAPI *pfnInit)(void* pCtx);
        uint64_t counter;
        void* pInterface;
    };

    auto* pData = reinterpret_cast<ContextInitData*>(pContextInitData);
    if (pData->pfnInit && !pData->pInterface) {
        LOGI("SteamInternal_ContextInit: Thuc thi pfnInit(%p)...", &pData->pInterface);
        pData->pfnInit(&pData->pInterface);
    }

    // Nếu sau khi gọi pfnInit mà pInterface vẫn là null, tự động gán Mock Steam Interface
    if (!pData->pInterface) {
        initSteamVtable();
        pData->pInterface = &gMockSteamInterface;
    }

    return &pData->pInterface;
}

const char* SteamHook_GetCurrentGameLanguage() {
    return stub_SteamApps_GetCurrentGameLanguage(nullptr);
}

}
