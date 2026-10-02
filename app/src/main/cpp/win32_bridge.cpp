#include "win32_bridge.h"
#include <android/log.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <chrono>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "SGRE_Win32Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// Cấu trúc chuỗi môi trường và command line Windows
static const wchar_t gCmdLineW[] = L"sgre_steam.exe";
static const char gCmdLineA[] = "sgre_steam.exe";
static const wchar_t gEnvStringsW[] = L"ALLUSERSPROFILE=C:\\ProgramData\0APPDATA=C:\\AppData\0\0";

// Handle giả định an toàn cho Windows
static void* const DUMMY_HEAP = reinterpret_cast<void*>(0x10001000);
static void* const DUMMY_MODULE = reinterpret_cast<void*>(0x20002000);
static void* const DUMMY_WINDOW = reinterpret_cast<void*>(0x30003000);
static void* const DUMMY_DC = reinterpret_cast<void*>(0x40004000);

// Biến lưu ImageBase thực tế trong RAM
static void* gLoadedImageBase = nullptr;

void Win32Bridge_SetLoadedImageBase(void* imageBase) {
    gLoadedImageBase = imageBase;
    LOGI("Win32Bridge đã thiết lập Loaded ImageBase: %p", imageBase);
}

// Generic fallback stub cho các hàm chưa dùng tới
int64_t WINAPI stub_GenericSuccess() { return 0; } // S_OK hoặc return 0 / NULL

// Các hàm Stub KERNEL32
const wchar_t* WINAPI stub_GetCommandLineW() { return gCmdLineW; }
const char* WINAPI stub_GetCommandLineA() { return gCmdLineA; }
const wchar_t* WINAPI stub_GetEnvironmentStringsW() { return gEnvStringsW; }
int32_t WINAPI stub_FreeEnvironmentStringsW(const wchar_t*) { return 1; }

uint32_t WINAPI stub_GetOEMCP() { return 65001; } // UTF-8
uint32_t WINAPI stub_GetACP() { return 65001; }
int32_t WINAPI stub_IsValidCodePage(uint32_t) { return 1; }
uint32_t WINAPI stub_GetConsoleOutputCP() { return 65001; }

void* WINAPI stub_GetProcessHeap() { return DUMMY_HEAP; }
void* WINAPI stub_HeapAlloc(void*, uint32_t, size_t size) { return malloc(size); }
void* WINAPI stub_HeapReAlloc(void*, uint32_t, void* ptr, size_t size) { return realloc(ptr, size); }
int32_t WINAPI stub_HeapFree(void*, uint32_t, void* ptr) { free(ptr); return 1; }
size_t WINAPI stub_HeapSize(void*, uint32_t, const void*) { return 0; }

uint32_t WINAPI stub_GetCurrentProcessId() { return static_cast<uint32_t>(getpid()); }
uint32_t WINAPI stub_GetCurrentThreadId() { return static_cast<uint32_t>(pthread_self()); }
void* WINAPI stub_GetCurrentProcess() { return reinterpret_cast<void*>(-1); }

void* WINAPI stub_GetModuleHandleA(const char* lpModuleName) {
    if (!lpModuleName || lpModuleName[0] == '\0') {
        return gLoadedImageBase ? gLoadedImageBase : DUMMY_MODULE;
    }
    return DUMMY_MODULE;
}

void* WINAPI stub_GetModuleHandleW(const wchar_t* lpModuleName) {
    if (!lpModuleName || lpModuleName[0] == L'\0') {
        return gLoadedImageBase ? gLoadedImageBase : DUMMY_MODULE;
    }
    return DUMMY_MODULE;
}

void* WINAPI stub_GetModuleHandleExW(uint32_t, const wchar_t* lpModuleName, void** phModule) {
    void* hMod = stub_GetModuleHandleW(lpModuleName);
    if (phModule) *phModule = hMod;
    return hMod;
}

void* WINAPI stub_LoadLibraryA(const char*) { return DUMMY_MODULE; }
void* WINAPI stub_LoadLibraryExW(const wchar_t*, void*, uint32_t) { return DUMMY_MODULE; }
int32_t WINAPI stub_FreeLibrary(void*) { return 1; }

void* WINAPI stub_GetProcAddress(void*, const char* lpProcName) {
    if (!lpProcName) return nullptr;
    LOGI("GetProcAddress dynamic query: %s", lpProcName);
    return Win32Bridge_ResolveSymbol(nullptr, lpProcName);
}

uint32_t WINAPI stub_GetTickCount() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t WINAPI stub_GetTickCount64() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

int32_t WINAPI stub_QueryPerformanceCounter(int64_t* lpPerformanceCount) {
    if (lpPerformanceCount) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        *lpPerformanceCount = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }
    return 1;
}

int32_t WINAPI stub_QueryPerformanceFrequency(int64_t* lpFrequency) {
    if (lpFrequency) {
        *lpFrequency = 1000000000LL; // 1 GHz frequency (nanoseconds)
    }
    return 1;
}

void WINAPI stub_Sleep(uint32_t dwMilliseconds) {
    usleep(dwMilliseconds * 1000);
}

void WINAPI stub_GetSystemTimeAsFileTime(void* lpSystemTimeAsFileTime) {
    if (lpSystemTimeAsFileTime) {
        // Chuyển đổi timestamp Unix (1970) sang Windows FILETIME (1601), đơn vị 100ns
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t t = (static_cast<uint64_t>(ts.tv_sec) * 10000000ULL) +
                     (static_cast<uint64_t>(ts.tv_nsec) / 100ULL) +
                     116444736000000000ULL;
        *reinterpret_cast<uint64_t*>(lpSystemTimeAsFileTime) = t;
    }
}

void WINAPI stub_GetSystemTimePreciseAsFileTime(void* lpSystemTimeAsFileTime) {
    stub_GetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}

int32_t WINAPI stub_SystemTimeToFileTime(const void*, void*) { return 1; }
int32_t WINAPI stub_FileTimeToSystemTime(const void*, void*) { return 1; }
int32_t WINAPI stub_FileTimeToLocalFileTime(const void*, void*) { return 1; }

uint32_t WINAPI stub_GetLastError() { return 0; }
void WINAPI stub_SetLastError(uint32_t) {}

void WINAPI stub_InitializeCriticalSection(void*) {}
int32_t WINAPI stub_InitializeCriticalSectionAndSpinCount(void*, uint32_t) { return 1; }
int32_t WINAPI stub_InitializeCriticalSectionEx(void*, uint32_t, uint32_t) { return 1; }
void WINAPI stub_EnterCriticalSection(void*) {}
void WINAPI stub_LeaveCriticalSection(void*) {}
void WINAPI stub_DeleteCriticalSection(void*) {}

void WINAPI stub_InitializeSRWLock(void*) {}
void WINAPI stub_AcquireSRWLockExclusive(void*) {}
void WINAPI stub_ReleaseSRWLockExclusive(void*) {}
void WINAPI stub_AcquireSRWLockShared(void*) {}
void WINAPI stub_ReleaseSRWLockShared(void*) {}
int32_t WINAPI stub_TryAcquireSRWLockExclusive(void*) { return 1; }

void WINAPI stub_InitializeConditionVariable(void*) {}
void WINAPI stub_WakeConditionVariable(void*) {}
void WINAPI stub_WakeAllConditionVariable(void*) {}
int32_t WINAPI stub_SleepConditionVariableSRW(void*, void*, uint32_t, uint32_t) { return 1; }

void WINAPI stub_InitializeSListHead(void*) {}

int32_t WINAPI stub_IsProcessorFeaturePresent(uint32_t processorFeature) {
    // PF_FASTFAIL_AVAILABLE = 23 (0x17) -> Trả về 0 để CRT không gọi __fastfail!
    if (processorFeature == 23) {
        return 0;
    }
    return 1;
}

int32_t WINAPI stub_IsDebuggerPresent() { return 0; }

uint32_t WINAPI stub_TlsAlloc() { return 1; }
void* WINAPI stub_TlsGetValue(uint32_t) { return nullptr; }
int32_t WINAPI stub_TlsSetValue(uint32_t, void*) { return 1; }
int32_t WINAPI stub_TlsFree(uint32_t) { return 1; }

uint32_t WINAPI stub_FlsAlloc(void*) { return 1; }
void* WINAPI stub_FlsGetValue(uint32_t) { return nullptr; }
int32_t WINAPI stub_FlsSetValue(uint32_t, void*) { return 1; }
int32_t WINAPI stub_FlsFree(uint32_t) { return 1; }

void* WINAPI stub_EncodePointer(void* ptr) { return ptr; }
void* WINAPI stub_DecodePointer(void* ptr) { return ptr; }

void WINAPI stub_OutputDebugStringW(const wchar_t*) {}
void WINAPI stub_GetStartupInfoW(void* lpStartupInfo) {
    if (lpStartupInfo) memset(lpStartupInfo, 0, 68);
}

// Exception Filter stubs
void* WINAPI stub_SetUnhandledExceptionFilter(void*) { return nullptr; }
int32_t WINAPI stub_UnhandledExceptionFilter(void*) { return 0; }
void WINAPI stub_RtlCaptureContext(void*) {}
void* WINAPI stub_RtlLookupFunctionEntry(uint64_t, void*, void*) { return nullptr; }
void* WINAPI stub_RtlVirtualUnwind(uint32_t, uint64_t, uint64_t, void*, void*, void*, void*, void*) { return nullptr; }
void WINAPI stub_TerminateProcess(void*, uint32_t) {}
void WINAPI stub_ExitProcess(uint32_t) {}

// Các hàm Stub USER32
void* WINAPI stub_CreateWindowExA(uint32_t, const char*, const char*, uint32_t, int, int, int, int, void*, void*, void*, void*) { return DUMMY_WINDOW; }
void* WINAPI stub_CreateWindowExW(uint32_t, const wchar_t*, const wchar_t*, uint32_t, int, int, int, int, void*, void*, void*, void*) { return DUMMY_WINDOW; }
int32_t WINAPI stub_DestroyWindow(void*) { return 1; }
int32_t WINAPI stub_ShowWindow(void*, int32_t) { return 1; }
int32_t WINAPI stub_SetWindowPos(void*, void*, int, int, int, int, uint32_t) { return 1; }
void* WINAPI stub_GetDC(void*) { return DUMMY_DC; }
int32_t WINAPI stub_ReleaseDC(void*, void*) { return 1; }
int32_t WINAPI stub_GetClientRect(void*, void* lpRect) {
    if (lpRect) {
        int32_t* r = reinterpret_cast<int32_t*>(lpRect);
        r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080;
    }
    return 1;
}
int32_t WINAPI stub_GetWindowRect(void*, void* lpRect) {
    if (lpRect) {
        int32_t* r = reinterpret_cast<int32_t*>(lpRect);
        r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080;
    }
    return 1;
}
int32_t WINAPI stub_GetSystemMetrics(int32_t nIndex) {
    if (nIndex == 0) return 1920; // SM_CXSCREEN
    if (nIndex == 1) return 1080; // SM_CYSCREEN
    return 0;
}
int32_t WINAPI stub_PeekMessageA(void*, void*, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t WINAPI stub_TranslateMessage(const void*) { return 1; }
int64_t WINAPI stub_DispatchMessageA(const void*) { return 0; }
int64_t WINAPI stub_DefWindowProcA(void*, uint32_t, uint64_t, int64_t) { return 0; }
void WINAPI stub_PostQuitMessage(int32_t) {}

// Bảng ánh xạ hàm API
static std::unordered_map<std::string, void*> gFunctionMap;

} // namespace

void Win32Bridge_Init() {
    if (!gFunctionMap.empty()) return;

    // KERNEL32 API Registration
    gFunctionMap["GetCommandLineW"] = reinterpret_cast<void*>(&stub_GetCommandLineW);
    gFunctionMap["GetCommandLineA"] = reinterpret_cast<void*>(&stub_GetCommandLineA);
    gFunctionMap["GetEnvironmentStringsW"] = reinterpret_cast<void*>(&stub_GetEnvironmentStringsW);
    gFunctionMap["FreeEnvironmentStringsW"] = reinterpret_cast<void*>(&stub_FreeEnvironmentStringsW);
    gFunctionMap["GetOEMCP"] = reinterpret_cast<void*>(&stub_GetOEMCP);
    gFunctionMap["GetACP"] = reinterpret_cast<void*>(&stub_GetACP);
    gFunctionMap["IsValidCodePage"] = reinterpret_cast<void*>(&stub_IsValidCodePage);
    gFunctionMap["GetConsoleOutputCP"] = reinterpret_cast<void*>(&stub_GetConsoleOutputCP);
    gFunctionMap["GetProcessHeap"] = reinterpret_cast<void*>(&stub_GetProcessHeap);
    gFunctionMap["HeapAlloc"] = reinterpret_cast<void*>(&stub_HeapAlloc);
    gFunctionMap["HeapReAlloc"] = reinterpret_cast<void*>(&stub_HeapReAlloc);
    gFunctionMap["HeapFree"] = reinterpret_cast<void*>(&stub_HeapFree);
    gFunctionMap["HeapSize"] = reinterpret_cast<void*>(&stub_HeapSize);
    gFunctionMap["GetCurrentProcessId"] = reinterpret_cast<void*>(&stub_GetCurrentProcessId);
    gFunctionMap["GetCurrentThreadId"] = reinterpret_cast<void*>(&stub_GetCurrentThreadId);
    gFunctionMap["GetCurrentProcess"] = reinterpret_cast<void*>(&stub_GetCurrentProcess);
    gFunctionMap["GetModuleHandleA"] = reinterpret_cast<void*>(&stub_GetModuleHandleA);
    gFunctionMap["GetModuleHandleW"] = reinterpret_cast<void*>(&stub_GetModuleHandleW);
    gFunctionMap["GetModuleHandleExW"] = reinterpret_cast<void*>(&stub_GetModuleHandleExW);
    gFunctionMap["GetProcAddress"] = reinterpret_cast<void*>(&stub_GetProcAddress);
    gFunctionMap["LoadLibraryA"] = reinterpret_cast<void*>(&stub_LoadLibraryA);
    gFunctionMap["LoadLibraryExW"] = reinterpret_cast<void*>(&stub_LoadLibraryExW);
    gFunctionMap["GetTickCount"] = reinterpret_cast<void*>(&stub_GetTickCount);
    gFunctionMap["GetTickCount64"] = reinterpret_cast<void*>(&stub_GetTickCount64);
    gFunctionMap["QueryPerformanceCounter"] = reinterpret_cast<void*>(&stub_QueryPerformanceCounter);
    gFunctionMap["QueryPerformanceFrequency"] = reinterpret_cast<void*>(&stub_QueryPerformanceFrequency);
    gFunctionMap["Sleep"] = reinterpret_cast<void*>(&stub_Sleep);
    gFunctionMap["GetSystemTimeAsFileTime"] = reinterpret_cast<void*>(&stub_GetSystemTimeAsFileTime);
    gFunctionMap["SystemTimeToFileTime"] = reinterpret_cast<void*>(&stub_SystemTimeToFileTime);
    gFunctionMap["FileTimeToSystemTime"] = reinterpret_cast<void*>(&stub_FileTimeToSystemTime);
    gFunctionMap["FileTimeToLocalFileTime"] = reinterpret_cast<void*>(&stub_FileTimeToLocalFileTime);
    gFunctionMap["GetLastError"] = reinterpret_cast<void*>(&stub_GetLastError);
    gFunctionMap["SetLastError"] = reinterpret_cast<void*>(&stub_SetLastError);
    gFunctionMap["InitializeCriticalSection"] = reinterpret_cast<void*>(&stub_InitializeCriticalSection);
    gFunctionMap["InitializeCriticalSectionAndSpinCount"] = reinterpret_cast<void*>(&stub_InitializeCriticalSectionAndSpinCount);
    gFunctionMap["InitializeCriticalSectionEx"] = reinterpret_cast<void*>(&stub_InitializeCriticalSectionEx);
    gFunctionMap["EnterCriticalSection"] = reinterpret_cast<void*>(&stub_EnterCriticalSection);
    gFunctionMap["LeaveCriticalSection"] = reinterpret_cast<void*>(&stub_LeaveCriticalSection);
    gFunctionMap["DeleteCriticalSection"] = reinterpret_cast<void*>(&stub_DeleteCriticalSection);
    gFunctionMap["InitializeSRWLock"] = reinterpret_cast<void*>(&stub_InitializeSRWLock);
    gFunctionMap["AcquireSRWLockExclusive"] = reinterpret_cast<void*>(&stub_AcquireSRWLockExclusive);
    gFunctionMap["ReleaseSRWLockExclusive"] = reinterpret_cast<void*>(&stub_ReleaseSRWLockExclusive);
    gFunctionMap["AcquireSRWLockShared"] = reinterpret_cast<void*>(&stub_AcquireSRWLockShared);
    gFunctionMap["ReleaseSRWLockShared"] = reinterpret_cast<void*>(&stub_ReleaseSRWLockShared);
    gFunctionMap["TryAcquireSRWLockExclusive"] = reinterpret_cast<void*>(&stub_TryAcquireSRWLockExclusive);
    gFunctionMap["InitializeConditionVariable"] = reinterpret_cast<void*>(&stub_InitializeConditionVariable);
    gFunctionMap["WakeConditionVariable"] = reinterpret_cast<void*>(&stub_WakeConditionVariable);
    gFunctionMap["WakeAllConditionVariable"] = reinterpret_cast<void*>(&stub_WakeAllConditionVariable);
    gFunctionMap["SleepConditionVariableSRW"] = reinterpret_cast<void*>(&stub_SleepConditionVariableSRW);
    gFunctionMap["InitializeSListHead"] = reinterpret_cast<void*>(&stub_InitializeSListHead);
    gFunctionMap["IsProcessorFeaturePresent"] = reinterpret_cast<void*>(&stub_IsProcessorFeaturePresent);
    gFunctionMap["IsDebuggerPresent"] = reinterpret_cast<void*>(&stub_IsDebuggerPresent);
    gFunctionMap["TlsAlloc"] = reinterpret_cast<void*>(&stub_TlsAlloc);
    gFunctionMap["TlsGetValue"] = reinterpret_cast<void*>(&stub_TlsGetValue);
    gFunctionMap["TlsSetValue"] = reinterpret_cast<void*>(&stub_TlsSetValue);
    gFunctionMap["TlsFree"] = reinterpret_cast<void*>(&stub_TlsFree);
    gFunctionMap["FlsAlloc"] = reinterpret_cast<void*>(&stub_FlsAlloc);
    gFunctionMap["FlsGetValue"] = reinterpret_cast<void*>(&stub_FlsGetValue);
    gFunctionMap["FlsSetValue"] = reinterpret_cast<void*>(&stub_FlsSetValue);
    gFunctionMap["FlsFree"] = reinterpret_cast<void*>(&stub_FlsFree);
    gFunctionMap["EncodePointer"] = reinterpret_cast<void*>(&stub_EncodePointer);
    gFunctionMap["DecodePointer"] = reinterpret_cast<void*>(&stub_DecodePointer);
    gFunctionMap["OutputDebugStringW"] = reinterpret_cast<void*>(&stub_OutputDebugStringW);
    gFunctionMap["GetStartupInfoW"] = reinterpret_cast<void*>(&stub_GetStartupInfoW);

    // USER32 API Registration
    gFunctionMap["CreateWindowExA"] = reinterpret_cast<void*>(&stub_CreateWindowExA);
    gFunctionMap["CreateWindowExW"] = reinterpret_cast<void*>(&stub_CreateWindowExW);
    gFunctionMap["DestroyWindow"] = reinterpret_cast<void*>(&stub_DestroyWindow);
    gFunctionMap["ShowWindow"] = reinterpret_cast<void*>(&stub_ShowWindow);
    gFunctionMap["SetWindowPos"] = reinterpret_cast<void*>(&stub_SetWindowPos);
    gFunctionMap["GetDC"] = reinterpret_cast<void*>(&stub_GetDC);
    gFunctionMap["ReleaseDC"] = reinterpret_cast<void*>(&stub_ReleaseDC);
    gFunctionMap["GetClientRect"] = reinterpret_cast<void*>(&stub_GetClientRect);
    gFunctionMap["GetWindowRect"] = reinterpret_cast<void*>(&stub_GetWindowRect);
    gFunctionMap["GetSystemMetrics"] = reinterpret_cast<void*>(&stub_GetSystemMetrics);
    gFunctionMap["PeekMessageA"] = reinterpret_cast<void*>(&stub_PeekMessageA);
    gFunctionMap["TranslateMessage"] = reinterpret_cast<void*>(&stub_TranslateMessage);
    gFunctionMap["DispatchMessageA"] = reinterpret_cast<void*>(&stub_DispatchMessageA);
    gFunctionMap["DefWindowProcA"] = reinterpret_cast<void*>(&stub_DefWindowProcA);
    gFunctionMap["PostQuitMessage"] = reinterpret_cast<void*>(&stub_PostQuitMessage);
    gFunctionMap["FreeLibrary"] = reinterpret_cast<void*>(&stub_FreeLibrary);
    gFunctionMap["GetSystemTimePreciseAsFileTime"] = reinterpret_cast<void*>(&stub_GetSystemTimePreciseAsFileTime);
    gFunctionMap["SetUnhandledExceptionFilter"] = reinterpret_cast<void*>(&stub_SetUnhandledExceptionFilter);
    gFunctionMap["UnhandledExceptionFilter"] = reinterpret_cast<void*>(&stub_UnhandledExceptionFilter);
    gFunctionMap["RtlCaptureContext"] = reinterpret_cast<void*>(&stub_RtlCaptureContext);
    gFunctionMap["RtlLookupFunctionEntry"] = reinterpret_cast<void*>(&stub_RtlLookupFunctionEntry);
    gFunctionMap["RtlVirtualUnwind"] = reinterpret_cast<void*>(&stub_RtlVirtualUnwind);
    gFunctionMap["TerminateProcess"] = reinterpret_cast<void*>(&stub_TerminateProcess);
    gFunctionMap["ExitProcess"] = reinterpret_cast<void*>(&stub_ExitProcess);

    LOGI("Bảng Win32 Bridge đã khởi tạo thành công %zu hàm stub native!", gFunctionMap.size());
}

void* Win32Bridge_ResolveSymbol(const char* dllName, const char* funcName) {
    if (!funcName) return nullptr;

    Win32Bridge_Init();

    auto it = gFunctionMap.find(funcName);
    if (it != gFunctionMap.end()) {
        return it->second;
    }

    // Nếu đây là truy vấn từ stub_GetProcAddress (dllName == nullptr), trả về nullptr
    // để CRT Windows biết API không được hỗ trợ và tự động fallback về API cũ
    if (dllName == nullptr) {
        LOGW("GetProcAddress: API '%s' chưa được triển khai, trả về nullptr cho caller", funcName);
        return nullptr;
    }

    // Fallback cho IAT resolving tĩnh: Trả về stub_GenericSuccess để tránh crash null-pointer dereference
    return reinterpret_cast<void*>(&stub_GenericSuccess);
}
