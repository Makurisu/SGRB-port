#include <cstdint>
#include <android/log.h>
#include <cstring>

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

// Bảng VTABLE chung cho mọi Steam Interface (ISteamUser, ISteamFriends, ISteamApps...)
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
    LOGI("SteamAPI_RestartAppIfNecessary called -> False (Không cần restart qua Steam)");
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
    initSteamVtable();
    // Trả về địa chỉ của đối tượng mock (mà ô đầu tiên trỏ tới bảng hàm ảo gSteamVtable)
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
        LOGI("SteamInternal_ContextInit: Thực thi pfnInit(%p)...", &pData->pInterface);
        pData->pfnInit(&pData->pInterface);
    }

    // Nếu sau khi gọi pfnInit mà pInterface vẫn là null, tự động gán Mock Steam Interface
    if (!pData->pInterface) {
        initSteamVtable();
        pData->pInterface = &gMockSteamInterface;
    }

    // Trả về địa chỉ của pInterface để khi caller làm `mov rcx, [rax]`, rcx sẽ là con trỏ interface hợp lệ
    return &pData->pInterface;
}

}
