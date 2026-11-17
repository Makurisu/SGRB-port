#include "win32_bridge.h"
#include "d3d11_bridge.h"
#include "dinput_bridge.h"
#include "xaudio2_bridge.h"
#include "media_foundation_bridge.h"
#include "audio_sink.h"
#include <android/log.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <algorithm>
#include <queue>
#include <cstdlib>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <uchar.h>
#include <sys/syscall.h>
#include <condition_variable>
#include <shared_mutex>
#include <fnmatch.h>
#include <ctime>

extern "C" const char* SteamHook_GetCurrentGameLanguage();

#if defined(__x86_64__)
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif
#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1001
#endif
#ifndef ARCH_GET_GS
#define ARCH_GET_GS 0x1004
#endif
#endif

#define LOG_TAG "SGRE_Win32Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Cấu trúc chuỗi môi trường và command line Windows (chuẩn UTF-16LE 2-byte)
static const char16_t gCmdLineW[] = u"sgre_steam.exe";
static const char gCmdLineA[] = "sgre_steam.exe";
static const char16_t gEnvStringsW[] = u"ALLUSERSPROFILE=C:\\ProgramData\0APPDATA=C:\\AppData\0\0";

// Handle giả định an toàn cho Windows
static void* const DUMMY_HEAP = reinterpret_cast<void*>(0x10001000);
static void* const DUMMY_MODULE = reinterpret_cast<void*>(0x20002000);
static void* const DUMMY_WINDOW = reinterpret_cast<void*>(0x30003000);
static void* const DUMMY_DC = reinterpret_cast<void*>(0x40004000);

// Biến lưu ImageBase thực tế trong RAM và TLS info
static void* gLoadedImageBase = nullptr;
static uint32_t gTlsRva = 0;
static uint64_t gOriginalImageBase = 0;

} // namespace

extern "C" void Win32Bridge_SetTlsInfo(uint32_t tlsRva, uint64_t origImageBase) {
    gTlsRva = tlsRva;
    gOriginalImageBase = origImageBase;
}

extern "C" bool Win32Bridge_SetupCurrentThreadTEB() {
#if defined(__x86_64__)
    void* memory = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        LOGE("Không thể mmap bộ nhớ cho Windows TEB/PEB: errno=%d", errno);
        return false;
    }
    memset(memory, 0, 65536);

    uint8_t* teb = static_cast<uint8_t*>(memory);
    uint8_t* peb = teb + 8192;
    uint8_t* params = peb + 4096;
    uint64_t* tlsArray = reinterpret_cast<uint64_t*>(params + 4096);
    uint8_t* tlsData = reinterpret_cast<uint8_t*>(tlsArray + 128);

    *reinterpret_cast<uint64_t*>(teb + 0x30) = reinterpret_cast<uint64_t>(teb);
    *reinterpret_cast<uint64_t*>(teb + 0x40) = static_cast<uint64_t>(getpid());
    *reinterpret_cast<uint64_t*>(teb + 0x48) = static_cast<uint64_t>(pthread_self());
    *reinterpret_cast<uint64_t*>(teb + 0x58) = reinterpret_cast<uint64_t>(tlsArray);
    *reinterpret_cast<uint64_t*>(teb + 0x60) = reinterpret_cast<uint64_t>(peb);

    tlsArray[0] = reinterpret_cast<uint64_t>(tlsData);

    if (gTlsRva != 0 && gLoadedImageBase != nullptr) {
        uint8_t* basePtr = static_cast<uint8_t*>(gLoadedImageBase);
        struct ImageTlsDirectory64 {
            uint64_t startAddressOfRawData;
            uint64_t endAddressOfRawData;
            uint64_t addressOfIndex;
            uint64_t addressOfCallBacks;
            uint32_t sizeOfZeroFill;
            uint32_t characteristics;
        };
        auto* tlsDir = reinterpret_cast<ImageTlsDirectory64*>(basePtr + gTlsRva);
        int64_t delta = reinterpret_cast<intptr_t>(gLoadedImageBase) - static_cast<int64_t>(gOriginalImageBase);

        if (tlsDir->addressOfIndex != 0) {
            uint32_t* pIndex = reinterpret_cast<uint32_t*>(tlsDir->addressOfIndex);
            if (reinterpret_cast<uintptr_t>(pIndex) < reinterpret_cast<uintptr_t>(gLoadedImageBase)) {
                pIndex = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pIndex) + delta);
            }
            *pIndex = 0;
        }

        if (tlsDir->startAddressOfRawData != 0 && tlsDir->endAddressOfRawData > tlsDir->startAddressOfRawData) {
            uint8_t* rawStart = reinterpret_cast<uint8_t*>(tlsDir->startAddressOfRawData);
            uint8_t* rawEnd = reinterpret_cast<uint8_t*>(tlsDir->endAddressOfRawData);
            if (reinterpret_cast<uintptr_t>(rawStart) < reinterpret_cast<uintptr_t>(gLoadedImageBase)) {
                rawStart += delta;
                rawEnd += delta;
            }
            size_t rawSize = rawEnd - rawStart;
            if (rawSize < 32768) {
                memcpy(tlsData, rawStart, rawSize);
            }
        }
    }

    *reinterpret_cast<uint64_t*>(peb + 0x10) = reinterpret_cast<uint64_t>(gLoadedImageBase);
    *reinterpret_cast<uint64_t*>(peb + 0x20) = reinterpret_cast<uint64_t>(params);
    *reinterpret_cast<uint32_t*>(params + 0x00) = 4096;
    *reinterpret_cast<uint32_t*>(params + 0x04) = 512;
    *reinterpret_cast<uint32_t*>(params + 0x08) = 0x00000001;

    long ret = syscall(SYS_arch_prctl, ARCH_SET_GS, teb);
    if (ret != 0) {
        LOGE("syscall(SYS_arch_prctl, ARCH_SET_GS) thất bại: ret=%ld, errno=%d", ret, errno);
        return false;
    }
    return true;
#else
    return true;
#endif
}

namespace {

// Generic fallback stub cho các hàm chưa dùng tới
int64_t WINAPI stub_GenericSuccess() { return 0; } // S_OK hoặc return 0 / NULL

// Các hàm Stub KERNEL32
const char16_t* WINAPI stub_GetCommandLineW() { return gCmdLineW; }
const char* WINAPI stub_GetCommandLineA() { return gCmdLineA; }
const char16_t* WINAPI stub_GetEnvironmentStringsW() { return gEnvStringsW; }
int32_t WINAPI stub_FreeEnvironmentStringsW(const char16_t*) { return 1; }

uint32_t WINAPI stub_GetOEMCP() { return 65001; } // UTF-8
uint32_t WINAPI stub_GetACP() { return 65001; }
int32_t WINAPI stub_IsValidCodePage(uint32_t) { return 1; }
uint32_t WINAPI stub_GetConsoleOutputCP() { return 65001; }

// Bảng theo dõi cấp phát Heap an toàn để chống crash SIGSEGV khi free con trỏ lạ
static std::unordered_map<uintptr_t, size_t> gHeapAllocations;
static std::mutex gHeapMutex;

// Bảng theo dõi bộ nhớ ảo VirtualAlloc
static std::unordered_map<uintptr_t, size_t> gVirtualAllocations;
static std::mutex gVirtualMutex;

void* WINAPI stub_GetProcessHeap() { return DUMMY_HEAP; }

void* WINAPI stub_HeapAlloc(void*, uint32_t dwFlags, size_t size) {
    if (size == 0) size = 1;
    void* ptr = malloc(size);
    if (!ptr) return nullptr;
    if (dwFlags & 0x08) { // HEAP_ZERO_MEMORY
        memset(ptr, 0, size);
    }
    std::lock_guard<std::mutex> lock(gHeapMutex);
    gHeapAllocations[reinterpret_cast<uintptr_t>(ptr)] = size;
    return ptr;
}

void* WINAPI stub_HeapReAlloc(void* hHeap, uint32_t dwFlags, void* ptr, size_t size) {
    if (!ptr) return stub_HeapAlloc(hHeap, dwFlags, size);
    if (size == 0) size = 1;

    std::lock_guard<std::mutex> lock(gHeapMutex);
    auto it = gHeapAllocations.find(reinterpret_cast<uintptr_t>(ptr));
    if (it != gHeapAllocations.end()) {
        size_t oldSize = it->second;
        gHeapAllocations.erase(it);
        void* newPtr = realloc(ptr, size);
        if (newPtr) {
            if ((dwFlags & 0x08) && size > oldSize) {
                memset(reinterpret_cast<char*>(newPtr) + oldSize, 0, size - oldSize);
            }
            gHeapAllocations[reinterpret_cast<uintptr_t>(newPtr)] = size;
            return newPtr;
        }
        gHeapAllocations[reinterpret_cast<uintptr_t>(ptr)] = oldSize;
        return nullptr;
    }

    void* newPtr = malloc(size);
    if (newPtr) {
        if (dwFlags & 0x08) memset(newPtr, 0, size);
        gHeapAllocations[reinterpret_cast<uintptr_t>(newPtr)] = size;
    }
    return newPtr;
}

int32_t WINAPI stub_HeapFree(void*, uint32_t, void* ptr) {
    if (!ptr) return 1;
    std::lock_guard<std::mutex> lock(gHeapMutex);
    auto it = gHeapAllocations.find(reinterpret_cast<uintptr_t>(ptr));
    if (it != gHeapAllocations.end()) {
        gHeapAllocations.erase(it);
        free(ptr);
        return 1;
    }
    LOGW("stub_HeapFree: bỏ qua con trỏ không hợp lệ hoặc không thuộc heap: %p", ptr);
    return 1;
}

size_t WINAPI stub_HeapSize(void*, uint32_t, const void* ptr) {
    if (!ptr) return 0;
    std::lock_guard<std::mutex> lock(gHeapMutex);
    auto it = gHeapAllocations.find(reinterpret_cast<uintptr_t>(ptr));
    if (it != gHeapAllocations.end()) return it->second;
    return 0;
}

void* WINAPI stub_HeapCreate(uint32_t, size_t, size_t) { return DUMMY_HEAP; }
int32_t WINAPI stub_HeapDestroy(void*) { return 1; }
int32_t WINAPI stub_HeapValidate(void*, uint32_t, const void*) { return 1; }

// Memory management: VirtualAlloc, VirtualFree, VirtualProtect, VirtualQuery
void* WINAPI stub_VirtualAlloc(void* lpAddress, size_t dwSize, uint32_t, uint32_t flProtect) {
    if (dwSize == 0) return nullptr;
    size_t alignedSize = (dwSize + 4095) & ~4095ULL;

    int prot = PROT_READ | PROT_WRITE;
    if (flProtect == 0x10 || flProtect == 0x20 || flProtect == 0x40) {
        prot |= PROT_EXEC;
    }

    void* addr = mmap(lpAddress, alignedSize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        LOGW("VirtualAlloc mmap thất bại cho kích thước: %zu", dwSize);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gVirtualMutex);
        gVirtualAllocations[reinterpret_cast<uintptr_t>(addr)] = alignedSize;
    }
    return addr;
}

int32_t WINAPI stub_VirtualFree(void* lpAddress, size_t dwSize, uint32_t) {
    if (!lpAddress) return 1;

    std::lock_guard<std::mutex> lock(gVirtualMutex);
    auto it = gVirtualAllocations.find(reinterpret_cast<uintptr_t>(lpAddress));
    if (it != gVirtualAllocations.end()) {
        size_t size = it->second;
        gVirtualAllocations.erase(it);
        munmap(lpAddress, size);
        return 1;
    }
    if (dwSize > 0) {
        munmap(lpAddress, (dwSize + 4095) & ~4095ULL);
    }
    return 1;
}

int32_t WINAPI stub_VirtualProtect(void* lpAddress, size_t dwSize, uint32_t flNewProtect, uint32_t* lpflOldProtect) {
    if (!lpAddress || dwSize == 0) return 0;
    if (lpflOldProtect) *lpflOldProtect = 0x40; // PAGE_EXECUTE_READWRITE

    int prot = PROT_READ | PROT_WRITE;
    if (flNewProtect == 0x10 || flNewProtect == 0x20 || flNewProtect == 0x40) {
        prot |= PROT_EXEC;
    }
    uintptr_t pageStart = reinterpret_cast<uintptr_t>(lpAddress) & ~4095ULL;
    size_t alignedLen = ((reinterpret_cast<uintptr_t>(lpAddress) + dwSize + 4095) & ~4095ULL) - pageStart;
    mprotect(reinterpret_cast<void*>(pageStart), alignedLen, prot);
    return 1;
}

size_t WINAPI stub_VirtualQuery(const void* lpAddress, void* lpBuffer, size_t dwLength) {
    if (!lpBuffer || dwLength < 48) return 0;
    struct MEM_INFO {
        uint64_t BaseAddress;
        uint64_t AllocationBase;
        uint32_t AllocationProtect;
        uint32_t Alignment1;
        uint64_t RegionSize;
        uint32_t State;
        uint32_t Protect;
        uint32_t Type;
        uint32_t Alignment2;
    };
    MEM_INFO* mi = reinterpret_cast<MEM_INFO*>(lpBuffer);
    uint64_t page = reinterpret_cast<uint64_t>(lpAddress) & ~4095ULL;
    mi->BaseAddress = page;
    mi->AllocationBase = page;
    mi->AllocationProtect = 0x40; // PAGE_EXECUTE_READWRITE
    mi->Alignment1 = 0;
    mi->RegionSize = 0x10000; // 64 KB
    mi->State = 0x1000; // MEM_COMMIT
    mi->Protect = 0x40;
    mi->Type = 0x20000; // MEM_PRIVATE
    mi->Alignment2 = 0;
    return sizeof(MEM_INFO);
}

// Global & Local memory
void* WINAPI stub_GlobalAlloc(uint32_t uFlags, size_t dwBytes) {
    if (dwBytes == 0) dwBytes = 1;
    void* ptr = malloc(dwBytes);
    if (ptr && (uFlags & 0x0040)) { // GMEM_ZEROINIT
        memset(ptr, 0, dwBytes);
    }
    return ptr;
}
void* WINAPI stub_GlobalFree(void* hMem) {
    if (hMem) free(hMem);
    return nullptr;
}
void* WINAPI stub_LocalFree(void* hMem) {
    if (hMem) free(hMem);
    return nullptr;
}

// System Information
void WINAPI stub_GetSystemInfo(void* lpSystemInfo) {
    if (!lpSystemInfo) return;
    struct SYS_INFO {
        uint16_t wProcessorArchitecture;
        uint16_t wReserved;
        uint32_t dwPageSize;
        void* lpMinimumApplicationAddress;
        void* lpMaximumApplicationAddress;
        uint64_t dwActiveProcessorMask;
        uint32_t dwNumberOfProcessors;
        uint32_t dwProcessorType;
        uint32_t dwAllocationGranularity;
        uint16_t wProcessorLevel;
        uint16_t wProcessorRevision;
    };
    SYS_INFO* si = reinterpret_cast<SYS_INFO*>(lpSystemInfo);
    memset(si, 0, sizeof(SYS_INFO));
    si->wProcessorArchitecture = 9; // PROCESSOR_ARCHITECTURE_AMD64
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = reinterpret_cast<void*>(0x10000);
    si->lpMaximumApplicationAddress = reinterpret_cast<void*>(0x7FFFFFFEFFFFULL);
    si->dwActiveProcessorMask = 0xF;
    si->dwNumberOfProcessors = 4;
    si->dwProcessorType = 8664;
    si->dwAllocationGranularity = 65536;
}
void WINAPI stub_GetNativeSystemInfo(void* lpSystemInfo) {
    stub_GetSystemInfo(lpSystemInfo);
}

static size_t utf16Len(const char16_t* s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len] != 0) ++len;
    return len;
}

// String conversions (UTF-16LE 2-byte char16_t)
static int32_t utf8ToUtf16(const char* src, int32_t srcLen, char16_t* dst, int32_t dstMax) {
    if (!src || srcLen <= 0) return 0;
    int32_t outCount = 0;
    int32_t i = 0;
    while (i < srcLen) {
        uint32_t cp = 0;
        uint8_t b0 = static_cast<uint8_t>(src[i]);
        if (b0 < 0x80) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= srcLen) { cp = b0; i += 1; }
            else {
                uint8_t b1 = static_cast<uint8_t>(src[i + 1]);
                if ((b1 & 0xC0) == 0x80) {
                    cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
                    i += 2;
                } else { cp = b0; i += 1; }
            }
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= srcLen) { cp = b0; i += 1; }
            else {
                uint8_t b1 = static_cast<uint8_t>(src[i + 1]);
                uint8_t b2 = static_cast<uint8_t>(src[i + 2]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                    cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    i += 3;
                } else { cp = b0; i += 1; }
            }
        } else if ((b0 & 0xF8) == 0xF0) {
            if (i + 3 >= srcLen) { cp = b0; i += 1; }
            else {
                uint8_t b1 = static_cast<uint8_t>(src[i + 1]);
                uint8_t b2 = static_cast<uint8_t>(src[i + 2]);
                uint8_t b3 = static_cast<uint8_t>(src[i + 3]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    i += 4;
                } else { cp = b0; i += 1; }
            }
        } else {
            cp = b0;
            i += 1;
        }

        if (cp <= 0xFFFF) {
            if (dst && outCount < dstMax) {
                dst[outCount] = static_cast<char16_t>(cp);
            }
            outCount++;
        } else {
            cp -= 0x10000;
            if (dst && outCount < dstMax) {
                dst[outCount] = static_cast<char16_t>(0xD800 + (cp >> 10));
            }
            outCount++;
            if (dst && outCount < dstMax) {
                dst[outCount] = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            }
            outCount++;
        }
    }
    return outCount;
}

static int32_t utf16ToUtf8(const char16_t* src, int32_t srcLen, char* dst, int32_t dstMax) {
    if (!src || srcLen <= 0) return 0;
    int32_t outCount = 0;
    int32_t i = 0;
    while (i < srcLen) {
        uint32_t cp = src[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && i < srcLen) {
            uint32_t low = src[i];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }

        if (cp <= 0x7F) {
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(cp);
            outCount++;
        } else if (cp <= 0x7FF) {
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0xC0 | (cp >> 6));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | (cp & 0x3F));
            outCount++;
        } else if (cp <= 0xFFFF) {
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0xE0 | (cp >> 12));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | (cp & 0x3F));
            outCount++;
        } else {
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0xF0 | (cp >> 18));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            outCount++;
            if (dst && outCount < dstMax) dst[outCount] = static_cast<char>(0x80 | (cp & 0x3F));
            outCount++;
        }
    }
    return outCount;
}

int32_t WINAPI stub_MultiByteToWideChar(uint32_t, uint32_t, const char* lpMultiByteStr, int32_t cbMultiByte, char16_t* lpWideCharStr, int32_t cchWideChar) {
    if (!lpMultiByteStr) return 0;
    int32_t len = cbMultiByte >= 0 ? cbMultiByte : static_cast<int32_t>(strlen(lpMultiByteStr));
    if (len == 0) {
        if (cchWideChar > 0 && lpWideCharStr) {
            lpWideCharStr[0] = u'\0';
        }
        return (cbMultiByte < 0) ? 1 : 0;
    }

    int32_t needed = utf8ToUtf16(lpMultiByteStr, len, lpWideCharStr, cchWideChar);
    if (cbMultiByte < 0) {
        if (cchWideChar > 0 && lpWideCharStr && needed < cchWideChar) {
            lpWideCharStr[needed] = u'\0';
        }
        needed++;
    }
    return (cchWideChar == 0) ? needed : std::min(needed, cchWideChar);
}

int32_t WINAPI stub_WideCharToMultiByte(uint32_t, uint32_t, const char16_t* lpWideCharStr, int32_t cchWideChar, char* lpMultiByteStr, int32_t cbMultiByte, const char*, int32_t*) {
    if (!lpWideCharStr) return 0;
    int32_t len = cchWideChar >= 0 ? cchWideChar : static_cast<int32_t>(utf16Len(lpWideCharStr));
    if (len == 0) {
        if (cbMultiByte > 0 && lpMultiByteStr) {
            lpMultiByteStr[0] = '\0';
        }
        return (cchWideChar < 0) ? 1 : 0;
    }

    int32_t needed = utf16ToUtf8(lpWideCharStr, len, lpMultiByteStr, cbMultiByte);
    if (cchWideChar < 0) {
        if (cbMultiByte > 0 && lpMultiByteStr && needed < cbMultiByte) {
            lpMultiByteStr[needed] = '\0';
        }
        needed++;
    }
    return (cbMultiByte == 0) ? needed : std::min(needed, cbMultiByte);
}

size_t WINAPI stub_lstrlenA(const char* lpString) {
    return lpString ? strlen(lpString) : 0;
}

char* WINAPI stub_lstrcpynA(char* lpString1, const char* lpString2, int32_t iMaxLength) {
    if (!lpString1 || iMaxLength <= 0) return lpString1;
    if (!lpString2) { lpString1[0] = '\0'; return lpString1; }
    strncpy(lpString1, lpString2, iMaxLength - 1);
    lpString1[iMaxLength - 1] = '\0';
    return lpString1;
}

// Win32 Threading Subsystem
using Win32ThreadStartRoutine = uint32_t (WINAPI *)(void* lpThreadParameter);

struct Win32ThreadInfo {
    pthread_t pth{0};
    uint32_t threadId{0};
    Win32ThreadStartRoutine startRoutine{nullptr};
    void* param{nullptr};
    uint32_t exitCode{0x103}; // STILL_ACTIVE
    bool finished{false};
    bool suspended{false};
    std::mutex suspendMutex;
    std::condition_variable suspendCond;
};

static std::mutex gThreadMutex;
static std::unordered_map<uintptr_t, std::shared_ptr<Win32ThreadInfo>> gThreadMap;
static uint32_t gNextThreadId = 1000;
static uintptr_t gNextThreadHandle = 0x20000;
static thread_local std::shared_ptr<Win32ThreadInfo> gCurrentWin32Thread;

// Forward declaration của bộ chuyển đổi chuỗi UTF-16
static std::string normalizeWinPathW(const char16_t* wpath);

// Hằng số trả về cho Win32 Wait Functions
constexpr uint32_t WIN32_WAIT_OBJECT_0 = 0x00000000;
constexpr uint32_t WIN32_WAIT_TIMEOUT  = 0x00000102;
constexpr uint32_t WIN32_WAIT_FAILED   = 0xFFFFFFFF;
constexpr uint32_t WIN32_INFINITE      = 0xFFFFFFFF;

// Win32 Synchronization Subsystem (Events & Semaphores)
enum class Win32SyncType {
    Event,
    Semaphore
};

struct Win32SyncObject {
    Win32SyncType type;
    std::string name;
    std::mutex mtx;
    std::condition_variable cv;

    // Cấu hình Event
    bool manualReset{false};
    bool signaled{false};

    // Cấu hình Semaphore
    int32_t currentCount{0};
    int32_t maxCount{0};
};

static std::mutex gSyncMutex;
static std::unordered_map<uintptr_t, std::shared_ptr<Win32SyncObject>> gSyncMap;
static uintptr_t gNextSyncHandle = 0x50000;

void* WINAPI stub_CreateEventA(void*, int32_t bManualReset, int32_t bInitialState, const char* lpName) {
    auto sync = std::make_shared<Win32SyncObject>();
    sync->type = Win32SyncType::Event;
    sync->manualReset = (bManualReset != 0);
    sync->signaled = (bInitialState != 0);
    if (lpName) sync->name = lpName;

    std::lock_guard<std::mutex> lock(gSyncMutex);
    uintptr_t handle = ++gNextSyncHandle;
    gSyncMap[handle] = sync;
    LOGI("stub_CreateEventA: handle=%p, manualReset=%d, initialState=%d, name='%s'",
         reinterpret_cast<void*>(handle), bManualReset, bInitialState, lpName ? lpName : "");
    return reinterpret_cast<void*>(handle);
}

void* WINAPI stub_CreateEventW(void* lpEventAttributes, int32_t bManualReset, int32_t bInitialState, const char16_t* lpName) {
    std::string nameStr;
    if (lpName) nameStr = normalizeWinPathW(lpName);
    return stub_CreateEventA(lpEventAttributes, bManualReset, bInitialState, nameStr.empty() ? nullptr : nameStr.c_str());
}

void* WINAPI stub_CreateEventExA(void* lpEventAttributes, const char* lpName, uint32_t dwFlags, uint32_t) {
    int32_t manualReset = (dwFlags & 0x00000001) != 0; // CREATE_EVENT_MANUAL_RESET
    int32_t initialState = (dwFlags & 0x00000002) != 0; // CREATE_EVENT_INITIAL_SET
    return stub_CreateEventA(lpEventAttributes, manualReset, initialState, lpName);
}

void* WINAPI stub_CreateEventExW(void* lpEventAttributes, const char16_t* lpName, uint32_t dwFlags, uint32_t dwDesiredAccess) {
    int32_t manualReset = (dwFlags & 0x00000001) != 0;
    int32_t initialState = (dwFlags & 0x00000002) != 0;
    return stub_CreateEventW(lpEventAttributes, manualReset, initialState, lpName);
}

int32_t WINAPI stub_SetEvent(void* hEvent) {
    std::shared_ptr<Win32SyncObject> sync;
    {
        std::lock_guard<std::mutex> lock(gSyncMutex);
        auto it = gSyncMap.find(reinterpret_cast<uintptr_t>(hEvent));
        if (it != gSyncMap.end()) {
            sync = it->second;
        }
    }
    if (!sync || sync->type != Win32SyncType::Event) return 0;

    {
        std::lock_guard<std::mutex> lock(sync->mtx);
        sync->signaled = true;
    }
    static uint32_t sSetEventLogCount = 0;
    if (!sync->name.empty() || ++sSetEventLogCount % 200 == 1) {
        LOGI("stub_SetEvent: handle=%p, name='%s', manual=%d", hEvent, sync->name.c_str(), sync->manualReset ? 1 : 0);
    }
    if (sync->manualReset) {
        sync->cv.notify_all();
    } else {
        sync->cv.notify_one();
    }
    return 1;
}

extern "C" int32_t Win32Bridge_SetEvent(void* hEvent) {
    return stub_SetEvent(hEvent);
}

int32_t WINAPI stub_ResetEvent(void* hEvent) {
    std::shared_ptr<Win32SyncObject> sync;
    {
        std::lock_guard<std::mutex> lock(gSyncMutex);
        auto it = gSyncMap.find(reinterpret_cast<uintptr_t>(hEvent));
        if (it != gSyncMap.end()) {
            sync = it->second;
        }
    }
    if (!sync || sync->type != Win32SyncType::Event) return 0;

    {
        std::lock_guard<std::mutex> lock(sync->mtx);
        sync->signaled = false;
    }
    static uint32_t sResetEventLog = 0;
    if (++sResetEventLog % 500 == 1) {
        LOGI("stub_ResetEvent #%u: handle=%p, name='%s'", sResetEventLog, hEvent, sync->name.c_str());
    }
    return 1;
}

void* WINAPI stub_CreateSemaphoreA(void*, int32_t lInitialCount, int32_t lMaximumCount, const char* lpName) {
    auto sync = std::make_shared<Win32SyncObject>();
    sync->type = Win32SyncType::Semaphore;
    sync->currentCount = lInitialCount;
    sync->maxCount = lMaximumCount;
    if (lpName) sync->name = lpName;

    std::lock_guard<std::mutex> lock(gSyncMutex);
    uintptr_t handle = ++gNextSyncHandle;
    gSyncMap[handle] = sync;
    LOGI("stub_CreateSemaphoreA: handle=%p, initialCount=%d, maxCount=%d, name='%s'",
         reinterpret_cast<void*>(handle), lInitialCount, lMaximumCount, lpName ? lpName : "");
    return reinterpret_cast<void*>(handle);
}

void* WINAPI stub_CreateSemaphoreW(void* lpSemaphoreAttributes, int32_t lInitialCount, int32_t lMaximumCount, const char16_t* lpName) {
    std::string nameStr;
    if (lpName) nameStr = normalizeWinPathW(lpName);
    return stub_CreateSemaphoreA(lpSemaphoreAttributes, lInitialCount, lMaximumCount, nameStr.empty() ? nullptr : nameStr.c_str());
}

int32_t WINAPI stub_ReleaseSemaphore(void* hSemaphore, int32_t lReleaseCount, int32_t* lpPreviousCount) {
    if (lReleaseCount <= 0) return 0;

    std::shared_ptr<Win32SyncObject> sync;
    {
        std::lock_guard<std::mutex> lock(gSyncMutex);
        auto it = gSyncMap.find(reinterpret_cast<uintptr_t>(hSemaphore));
        if (it != gSyncMap.end()) {
            sync = it->second;
        }
    }
    if (!sync || sync->type != Win32SyncType::Semaphore) return 0;

    int32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(sync->mtx);
        prev = sync->currentCount;
        if (lpPreviousCount) {
            *lpPreviousCount = prev;
        }
        if (sync->maxCount > 0 && sync->currentCount + lReleaseCount > sync->maxCount) {
            return 0; // Vượt quá giới hạn tối đa
        }
        sync->currentCount += lReleaseCount;
    }

    LOGI("stub_ReleaseSemaphore: handle=%p, count=%d (prev=%d) -> newCount=%d",
         hSemaphore, lReleaseCount, prev, prev + lReleaseCount);

    if (lReleaseCount == 1) {
        sync->cv.notify_one();
    } else {
        sync->cv.notify_all();
    }
    return 1;
}

uint32_t WINAPI stub_WaitForSingleObject(void* hHandle, uint32_t dwMilliseconds) {
    if (!hHandle) return WIN32_WAIT_FAILED;

    if (dwMilliseconds != 0) {
        static uint32_t sWaitLog = 0;
        if (++sWaitLog % 500 == 1) {
            LOGI("stub_WaitForSingleObject #%u: Chờ handle=%p với timeout=%u ms", sWaitLog, hHandle, dwMilliseconds);
        }
    }

    // 1. Kiểm tra nếu hHandle là thread handle
    std::shared_ptr<Win32ThreadInfo> threadInfo;
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        auto tit = gThreadMap.find(reinterpret_cast<uintptr_t>(hHandle));
        if (tit != gThreadMap.end()) {
            threadInfo = tit->second;
        }
    }

    if (threadInfo) {
        if (threadInfo->finished) {
            LOGI("stub_WaitForSingleObject: Thread handle=%p (TID=%u) đã finished (exitCode=%u) -> trả về WAIT_OBJECT_0",
                 hHandle, threadInfo->threadId, threadInfo->exitCode);
            return WIN32_WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0) return WIN32_WAIT_TIMEOUT;

        std::unique_lock<std::mutex> lock(threadInfo->suspendMutex);
        if (dwMilliseconds == WIN32_INFINITE) {
            threadInfo->suspendCond.wait(lock, [&] { return threadInfo->finished; });
            LOGI("stub_WaitForSingleObject: Thread handle=%p (TID=%u) đã nhận tín hiệu finished!", hHandle, threadInfo->threadId);
            return WIN32_WAIT_OBJECT_0;
        } else {
            bool ok = threadInfo->suspendCond.wait_for(
                lock,
                std::chrono::milliseconds(dwMilliseconds),
                [&] { return threadInfo->finished; }
            );
            if (ok) {
                LOGI("stub_WaitForSingleObject: Thread handle=%p (TID=%u) đã nhận tín hiệu finished trong timeout!", hHandle, threadInfo->threadId);
                return WIN32_WAIT_OBJECT_0;
            }
            return WIN32_WAIT_TIMEOUT;
        }
    }

    // 2. Kiểm tra nếu hHandle là đối tượng đồng bộ (Event, Semaphore)
    std::shared_ptr<Win32SyncObject> sync;
    {
        std::lock_guard<std::mutex> lock(gSyncMutex);
        auto it = gSyncMap.find(reinterpret_cast<uintptr_t>(hHandle));
        if (it != gSyncMap.end()) {
            sync = it->second;
        }
    }

    if (!sync) {
        // Handle chưa nhận dạng: trả về WAIT_OBJECT_0 để tương thích ngược với dummy handles
        return WIN32_WAIT_OBJECT_0;
    }

    std::unique_lock<std::mutex> lock(sync->mtx);
    if (sync->type == Win32SyncType::Event) {
        if (sync->signaled) {
            if (!sync->manualReset) {
                sync->signaled = false; // Auto-reset
            }
            return WIN32_WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0) return WIN32_WAIT_TIMEOUT;

        if (dwMilliseconds == WIN32_INFINITE) {
            sync->cv.wait(lock, [&] { return sync->signaled; });
            if (!sync->manualReset) {
                sync->signaled = false;
            }
            return WIN32_WAIT_OBJECT_0;
        } else {
            bool ok = sync->cv.wait_for(
                lock,
                std::chrono::milliseconds(dwMilliseconds),
                [&] { return sync->signaled; }
            );
            if (ok) {
                if (!sync->manualReset) {
                    sync->signaled = false;
                }
                return WIN32_WAIT_OBJECT_0;
            }
            return WIN32_WAIT_TIMEOUT;
        }
    } else if (sync->type == Win32SyncType::Semaphore) {
        if (sync->currentCount > 0) {
            sync->currentCount--;
            return WIN32_WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0) return WIN32_WAIT_TIMEOUT;

        if (dwMilliseconds == WIN32_INFINITE) {
            sync->cv.wait(lock, [&] { return sync->currentCount > 0; });
            sync->currentCount--;
            return WIN32_WAIT_OBJECT_0;
        } else {
            bool ok = sync->cv.wait_for(
                lock,
                std::chrono::milliseconds(dwMilliseconds),
                [&] { return sync->currentCount > 0; }
            );
            if (ok) {
                sync->currentCount--;
                return WIN32_WAIT_OBJECT_0;
            }
            return WIN32_WAIT_TIMEOUT;
        }
    }

    return WIN32_WAIT_FAILED;
}

uint32_t WINAPI stub_WaitForSingleObjectEx(void* hHandle, uint32_t dwMilliseconds, int32_t) {
    return stub_WaitForSingleObject(hHandle, dwMilliseconds);
}

int32_t WINAPI stub_SwitchToThread() { sched_yield(); return 1; }

uint32_t WINAPI stub_GetCurrentThreadId();

static void win32ThreadExitInternal(uint32_t exitCode) {
    std::shared_ptr<Win32ThreadInfo> info = gCurrentWin32Thread;
    if (!info) {
        uint32_t tid = stub_GetCurrentThreadId();
        std::lock_guard<std::mutex> lock(gThreadMutex);
        for (const auto& pair : gThreadMap) {
            if (pair.second && pair.second->threadId == tid) {
                info = pair.second;
                break;
            }
        }
    }
    if (info) {
        info->exitCode = exitCode;
        {
            std::lock_guard<std::mutex> sLock(info->suspendMutex);
            info->finished = true;
        }
        info->suspendCond.notify_all();
        LOGI("Win32 Worker Thread %u kết thúc với exitCode=%u (finished=true)", info->threadId, exitCode);
    }
    pthread_exit(nullptr);
}

static void* win32ThreadTrampoline(void* arg) {
    auto* holder = static_cast<std::shared_ptr<Win32ThreadInfo>*>(arg);
    std::shared_ptr<Win32ThreadInfo> info = *holder;
    delete holder;

    gCurrentWin32Thread = info;
    info->threadId = stub_GetCurrentThreadId();
    Win32Bridge_SetupCurrentThreadTEB();

    // Nếu thread được tạo với cờ CREATE_SUSPENDED (0x04), tạm dừng chờ ResumeThread()
    if (info->suspended) {
        LOGI("Win32 Worker Thread %u đang tạm dừng chờ ResumeThread...", info->threadId);
        std::unique_lock<std::mutex> lock(info->suspendMutex);
        info->suspendCond.wait(lock, [info] { return !info->suspended; });
        LOGI("Win32 Worker Thread %u đã nhận tín hiệu ResumeThread, bắt đầu chạy!", info->threadId);
    }

    LOGI("Win32 Worker Thread %u bắt đầu thực thi startRoutine=%p (ms_abi), param=%p",
         info->threadId, info->startRoutine, info->param);
    uint32_t exitCode = 0;
    if (info->startRoutine) {
        try {
            exitCode = info->startRoutine(info->param);
        } catch (...) {
            LOGE("Ngoại lệ xảy ra trong Win32 Worker Thread %u!", info->threadId);
            exitCode = 0xFFFFFFFF;
        }
    }
    win32ThreadExitInternal(exitCode);
    return nullptr;
}

void* WINAPI stub_CreateThread(
    void*,
    size_t dwStackSize,
    Win32ThreadStartRoutine lpStartAddress,
    void* lpParameter,
    uint32_t dwCreationFlags,
    uint32_t* lpThreadId
) {
    auto info = std::make_shared<Win32ThreadInfo>();
    uint32_t tid = 0;
    uintptr_t hThread = 0;
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        tid = ++gNextThreadId;
        hThread = ++gNextThreadHandle;
        info->threadId = tid;
        info->startRoutine = lpStartAddress;
        info->param = lpParameter;
        info->exitCode = 0x103; // STILL_ACTIVE
        info->finished = false;
        info->suspended = (dwCreationFlags & 0x00000004) != 0; // CREATE_SUSPENDED
        gThreadMap[hThread] = info;
    }
    if (lpThreadId) *lpThreadId = tid;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stackSize = (dwStackSize > 0) ? std::max<size_t>(dwStackSize, 1024 * 1024) : (2 * 1024 * 1024);
    pthread_attr_setstacksize(&attr, stackSize);

    auto* holder = new std::shared_ptr<Win32ThreadInfo>(info);
    int ret = pthread_create(&info->pth, &attr, win32ThreadTrampoline, holder);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        LOGE("stub_CreateThread: pthread_create thất bại! errno=%d", ret);
        delete holder;
        std::lock_guard<std::mutex> lock(gThreadMutex);
        gThreadMap.erase(hThread);
        return nullptr;
    }

    LOGI("stub_CreateThread: Khởi tạo Win32 Thread Handle=%p, ThreadID=%u, StartAddress=%p, suspended=%d",
         reinterpret_cast<void*>(hThread), tid, lpStartAddress, info->suspended ? 1 : 0);
    return reinterpret_cast<void*>(hThread);
}

uint32_t WINAPI stub_ResumeThread(void* hThread) {
    std::shared_ptr<Win32ThreadInfo> info;
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        auto it = gThreadMap.find(reinterpret_cast<uintptr_t>(hThread));
        if (it != gThreadMap.end()) {
            info = it->second;
        }
    }
    if (info) {
        if (info->suspended) {
            {
                std::lock_guard<std::mutex> sLock(info->suspendMutex);
                info->suspended = false;
            }
            info->suspendCond.notify_all();
            LOGI("stub_ResumeThread: Đã đánh thức Win32 Thread Handle=%p, ThreadID=%u", hThread, info->threadId);
            return 1; // Số lần suspend trước đó = 1
        }
    }
    return 0;
}
int32_t WINAPI stub_SetThreadPriority(void*, int32_t) { return 1; }
uint32_t WINAPI stub_SetThreadIdealProcessor(void*, uint32_t) { return 0; }

int32_t WINAPI stub_GetExitCodeThread(void* hThread, uint32_t* lpExitCode) {
    if (!lpExitCode) return 0;
    std::lock_guard<std::mutex> lock(gThreadMutex);
    auto it = gThreadMap.find(reinterpret_cast<uintptr_t>(hThread));
    if (it != gThreadMap.end()) {
        *lpExitCode = it->second->finished ? it->second->exitCode : 0x103;
        return 1;
    }
    *lpExitCode = 0;
    return 1;
}

int32_t WINAPI stub_TerminateThread(void* hThread, uint32_t exitCode) {
    std::shared_ptr<Win32ThreadInfo> info;
    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        auto it = gThreadMap.find(reinterpret_cast<uintptr_t>(hThread));
        if (it != gThreadMap.end()) {
            info = it->second;
        }
    }
    if (info) {
        info->exitCode = exitCode;
        {
            std::lock_guard<std::mutex> sLock(info->suspendMutex);
            info->finished = true;
        }
        info->suspendCond.notify_all();
        LOGI("stub_TerminateThread: Thread handle %p (TID=%u) bị terminate với exitCode=%u", hThread, info->threadId, exitCode);
        return 1;
    }
    return 0;
}

void WINAPI stub_ExitThread(uint32_t dwExitCode) {
    LOGI("stub_ExitThread: Gọi kết thúc thread với exitCode=%u", dwExitCode);
    win32ThreadExitInternal(dwExitCode);
}

void WINAPI stub_FreeLibraryAndExitThread(void* /* hLibModule */, uint32_t dwExitCode) {
    LOGI("stub_FreeLibraryAndExitThread: Gọi kết thúc thread với exitCode=%u", dwExitCode);
    win32ThreadExitInternal(dwExitCode);
}

int32_t WINAPI stub_GetProcessAffinityMask(void*, uint64_t* lpProcessAffinityMask, uint64_t* lpSystemAffinityMask) {
    if (lpProcessAffinityMask) *lpProcessAffinityMask = 0xF;
    if (lpSystemAffinityMask) *lpSystemAffinityMask = 0xF;
    return 1;
}

// Console & Standard Handles
void* WINAPI stub_GetStdHandle(uint32_t nStdHandle) {
    if (nStdHandle == static_cast<uint32_t>(-10)) return reinterpret_cast<void*>(0x10);
    if (nStdHandle == static_cast<uint32_t>(-11)) return reinterpret_cast<void*>(0x11);
    if (nStdHandle == static_cast<uint32_t>(-12)) return reinterpret_cast<void*>(0x12);
    return reinterpret_cast<void*>(-1);
}
int32_t WINAPI stub_SetStdHandle(uint32_t, void*) { return 1; }
uint32_t WINAPI stub_GetFileType(void*) { return 2; } // FILE_TYPE_CHAR
int32_t WINAPI stub_GetConsoleMode(void*, uint32_t* lpMode) {
    if (lpMode) *lpMode = 0x0007;
    return 1;
}
int32_t WINAPI stub_WriteConsoleW(void*, const void*, uint32_t nChars, uint32_t* lpWritten, void*) {
    if (lpWritten) *lpWritten = nChars;
    return 1;
}
int32_t WINAPI stub_ReadConsoleW(void*, void*, uint32_t, uint32_t* lpRead, void*) {
    if (lpRead) *lpRead = 0;
    return 1;
}

// ============================================================================
// VIRTUAL FILE SYSTEM (VFS) CHO WIN32 FILE I/O
// ============================================================================

struct Win32FileTime {
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
};

struct Win32ByHandleFileInformation {
    uint32_t dwFileAttributes;
    Win32FileTime ftCreationTime;
    Win32FileTime ftLastAccessTime;
    Win32FileTime ftLastWriteTime;
    uint32_t dwVolumeSerialNumber;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t nNumberOfLinks;
    uint32_t nFileIndexHigh;
    uint32_t nFileIndexLow;
};

struct Win32FindDataW {
    uint32_t dwFileAttributes;
    Win32FileTime ftCreationTime;
    Win32FileTime ftLastAccessTime;
    Win32FileTime ftLastWriteTime;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t dwReserved0;
    uint32_t dwReserved1;
    char16_t cFileName[260];
    char16_t cAlternateFileName[14];
};
static_assert(sizeof(Win32FindDataW) == 592, "Win32FindDataW must be exactly 592 bytes for Windows ABI");

struct Win32FindDataA {
    uint32_t dwFileAttributes;
    Win32FileTime ftCreationTime;
    Win32FileTime ftLastAccessTime;
    Win32FileTime ftLastWriteTime;
    uint32_t nFileSizeHigh;
    uint32_t nFileSizeLow;
    uint32_t dwReserved0;
    uint32_t dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
};

struct Win32Overlapped {
    uintptr_t internal;      // Offset 0x00: NTSTATUS / Error code (0 = STATUS_SUCCESS)
    uintptr_t internalHigh;  // Offset 0x08: Số byte đã truyền (Bytes transferred)
    union {
        struct {
            uint32_t offset;     // Offset 0x10: Vị trí đọc/ghi file (Low 32-bit)
            uint32_t offsetHigh; // Offset 0x14: Vị trí đọc/ghi file (High 32-bit)
        };
        void* pointer;
    };
    void* hEvent;            // Offset 0x18: Event handle đồng bộ
};
static_assert(sizeof(Win32Overlapped) == 32, "Win32Overlapped must be exactly 32 bytes on 64-bit Windows ABI");

struct VfsFileHandle {
    int fd;
    std::string path;
    uint32_t accessMode;
};

struct VfsFindEntry {
    std::string fileName;
    uint32_t attributes;
    uint32_t fileSizeHigh;
    uint32_t fileSizeLow;
    Win32FileTime ftCreationTime;
    Win32FileTime ftLastAccessTime;
    Win32FileTime ftLastWriteTime;
    time_t mtimeSec;
};

struct VfsFindState {
    std::vector<VfsFindEntry> entries;
    size_t currentIndex;
};

// Chuyển đổi timestamp Unix (1970) sang Windows FILETIME (1601) đơn vị 100ns
static void unixTimeToWin32FileTime(time_t sec, uint32_t nsec, Win32FileTime* ft) {
    if (!ft) return;
    uint64_t winTime = (static_cast<uint64_t>(sec) + 11644473600ULL) * 10000000ULL + (nsec / 100ULL);
    ft->dwLowDateTime = static_cast<uint32_t>(winTime & 0xFFFFFFFF);
    ft->dwHighDateTime = static_cast<uint32_t>(winTime >> 32);
}

static std::string gVfsDataDir = "/storage/emulated/0/Android/data/com.makurisu.sgrb/files/data";
static std::mutex gVfsMutex;
static std::string gCurrentVfsDirectory;
static std::mutex gCurrentDirMutex;
static std::unordered_map<uintptr_t, VfsFileHandle> gVfsHandles;
static std::unordered_map<uintptr_t, VfsFindState> gVfsFindStates;
static uintptr_t gNextHandleId = 0x10000;
static thread_local uint32_t gLastErrorCode = 0;
void WINAPI stub_SetLastError(uint32_t err);

static std::string normalizeWinPathW(const char16_t* wpath) {
    if (!wpath) return "";
    std::string s;
    for (size_t i = 0; wpath[i] != 0; ++i) {
        char16_t wc = wpath[i];
        if (wc == u'\\') {
            s.push_back('/');
        } else if (wc < 0x80) {
            s.push_back(static_cast<char>(wc));
        } else if (wc < 0x0800) {
            s.push_back(static_cast<char>(0xC0 | ((wc >> 6) & 0x1F)));
            s.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xE0 | ((wc >> 12) & 0x0F)));
            s.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    if (s.size() >= 2 && s[1] == ':') {
        s = s.substr(2);
    }
    while (!s.empty() && s[0] == '/') {
        s = s.substr(1);
    }
    return s;
}

static void copyUtf8ToUtf16(const std::string& in, char16_t* out, size_t maxChars) {
    if (!out || maxChars == 0) return;
    size_t outIdx = 0;
    size_t inIdx = 0;
    while (inIdx < in.size() && outIdx + 1 < maxChars) {
        unsigned char c = static_cast<unsigned char>(in[inIdx++]);
        if (c < 0x80) {
            out[outIdx++] = static_cast<char16_t>(c);
        } else if ((c & 0xE0) == 0xC0 && inIdx < in.size()) {
            unsigned char c2 = static_cast<unsigned char>(in[inIdx++]);
            out[outIdx++] = static_cast<char16_t>(((c & 0x1F) << 6) | (c2 & 0x3F));
        } else if ((c & 0xF0) == 0xE0 && inIdx + 1 < in.size()) {
            unsigned char c2 = static_cast<unsigned char>(in[inIdx++]);
            unsigned char c3 = static_cast<unsigned char>(in[inIdx++]);
            out[outIdx++] = static_cast<char16_t>(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
        } else {
            out[outIdx++] = static_cast<char16_t>(c);
        }
    }
    out[outIdx] = 0;
}

static std::string normalizeWinPathA(const char* apath) {
    if (!apath) return "";
    std::string s;
    for (size_t i = 0; apath[i] != 0; ++i) {
        char c = apath[i];
        if (c == '\\') s.push_back('/');
        else s.push_back(c);
    }
    if (s.size() >= 2 && s[1] == ':') {
        s = s.substr(2);
    }
    while (!s.empty() && s[0] == '/') {
        s = s.substr(1);
    }
    return s;
}

// Duyệt đường dẫn tương đối theo từng thành phần (subpath) case-insensitively từ baseDir
static std::string resolveCaseInsensitiveSubPath(const std::string& baseDir, const std::string& subPath) {
    if (subPath.empty()) return baseDir;

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < subPath.size()) {
        size_t end = subPath.find_first_of("/\\", start);
        if (end == std::string::npos) end = subPath.size();
        if (end > start) {
            std::string part = subPath.substr(start, end - start);
            if (part != "." && !part.empty()) {
                parts.push_back(part);
            }
        }
        start = end + 1;
    }

    std::string currentPath = baseDir;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& target = parts[i];
        std::string nextPath = currentPath + "/" + target;

        struct stat st;
        if (stat(nextPath.c_str(), &st) == 0) {
            currentPath = nextPath;
            continue;
        }

        DIR* dir = opendir(currentPath.c_str());
        if (!dir) {
            for (size_t j = i; j < parts.size(); ++j) {
                currentPath += "/" + parts[j];
            }
            return currentPath;
        }

        bool found = false;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcasecmp(entry->d_name, target.c_str()) == 0) {
                currentPath = currentPath + "/" + entry->d_name;
                found = true;
                break;
            }
        }
        closedir(dir);

        if (!found) {
            for (size_t j = i; j < parts.size(); ++j) {
                currentPath += "/" + parts[j];
            }
            break;
        }
    }
    return currentPath;
}

// Tìm kiếm đường dẫn theo cơ chế Case-Insensitive (vì Windows không phân biệt hoa thường, còn Android Linux thì có)
static std::string resolveCaseInsensitivePath(const std::string& rootDir, const std::string& relPath) {
    if (relPath.empty()) return rootDir;

    // 0. Language Redirection cho video cutscene trong thư mục movie:
    // Khi game cấu hình english (hoặc schinese/tchinese), các video có bản hardsub riêng (ví dụ prologue01_en.webm)
    // nhưng kịch bản scn/ks luôn yêu cầu tên gốc (prologue01.wmv hoặc prologue01.webm).
    bool isMovie = (relPath.find("movie") != std::string::npos) ||
                   (relPath.size() > 4 && (relPath.rfind(".wmv") == relPath.size() - 4 || relPath.rfind(".webm") == relPath.size() - 5));
    if (isMovie) {
        const char* curLang = SteamHook_GetCurrentGameLanguage();
        std::string langSuffix;
        if (curLang) {
            if (strcasecmp(curLang, "english") == 0 || strcasecmp(curLang, "en") == 0) langSuffix = "_en";
            else if (strcasecmp(curLang, "schinese") == 0) langSuffix = "_sc";
            else if (strcasecmp(curLang, "tchinese") == 0) langSuffix = "_tc";
        }

        size_t slashPos = relPath.rfind('/');
        std::string filename = (slashPos != std::string::npos) ? relPath.substr(slashPos + 1) : relPath;
        size_t dotPos = filename.rfind('.');
        std::string baseName = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;

        // Nếu file chưa có hậu tố ngôn ngữ, thử tìm bản ngôn ngữ _en.webm / _sc.webm / _tc.webm
        if (!langSuffix.empty() && baseName.rfind(langSuffix) != baseName.size() - langSuffix.size()) {
            std::string langWebmName = baseName + langSuffix + ".webm";
            const char* candidateSubdirs[] = {
                "wind3d11data/movie/",
                "movie/",
                ""
            };
            for (const char* sub : candidateSubdirs) {
                std::string testPath = rootDir + "/" + sub + langWebmName;
                struct stat st;
                if (stat(testPath.c_str(), &st) == 0) {
                    LOGI("VFS Video Redirect [Language: %s]: '%s' -> '%s'", curLang, relPath.c_str(), testPath.c_str());
                    return testPath;
                }
            }
        }

        // Nếu không có bản ngôn ngữ hoặc ngôn ngữ japanese, thử map .wmv sang .webm
        if (relPath.size() > 4 && relPath.rfind(".wmv") == relPath.size() - 4) {
            std::string webmName = baseName + ".webm";
            const char* candidateSubdirs[] = {
                "wind3d11data/movie/",
                "movie/",
                ""
            };
            for (const char* sub : candidateSubdirs) {
                std::string testPath = rootDir + "/" + sub + webmName;
                struct stat st;
                if (stat(testPath.c_str(), &st) == 0) {
                    LOGI("VFS Video Redirect [WMV->WEBM]: '%s' -> '%s'", relPath.c_str(), testPath.c_str());
                    return testPath;
                }
            }
        }
    }

    // 0.1. Tương thích ngược và định tuyến Save Data giữa 'saves/' và '<Documents>/...':
    std::string effectiveRelPath = relPath;
    if (effectiveRelPath.rfind("wind3d11data", 0) != 0 &&
        effectiveRelPath.rfind("windata", 0) != 0 &&
        effectiveRelPath.rfind("winweb", 0) != 0 &&
        effectiveRelPath.rfind("movie", 0) != 0 &&
        effectiveRelPath.rfind("saves", 0) != 0 &&
        effectiveRelPath.find("<Documents>") == std::string::npos &&
        (effectiveRelPath.empty() || effectiveRelPath[0] != '/')) {

        std::string curDir;
        {
            std::lock_guard<std::mutex> lock(gCurrentDirMutex);
            curDir = gCurrentVfsDirectory;
        }

        if (!curDir.empty()) {
            effectiveRelPath = curDir + "/" + effectiveRelPath;
        } else if (effectiveRelPath.rfind("data_", 0) == 0 || effectiveRelPath.rfind("meta_", 0) == 0) {
            effectiveRelPath = "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/" + effectiveRelPath;
        }
    }

    if (effectiveRelPath.find("<Documents>") != std::string::npos || effectiveRelPath.rfind("saves", 0) == 0) {
        std::string sub;
        size_t docPos = effectiveRelPath.find("<Documents>");
        if (docPos != std::string::npos) {
            sub = effectiveRelPath.substr(docPos + 11);
        } else {
            sub = effectiveRelPath.substr(5);
        }
        while (!sub.empty() && (sub[0] == '/' || sub[0] == '\\')) sub.erase(0, 1);

        std::string baseNew = rootDir + "/saves";
        std::string checkNew = resolveCaseInsensitiveSubPath(baseNew, sub);
        struct stat stNew;
        if (stat(checkNew.c_str(), &stNew) == 0) {
            return checkNew;
        }

        // Fallback đọc bản legacy ở <Documents> chỉ khi file chưa kịp chuyển sang saves/
        std::string baseOld = rootDir + "/<Documents>";
        std::string checkOld = resolveCaseInsensitiveSubPath(baseOld, sub);
        struct stat stOld;
        if (stat(checkOld.c_str(), &stOld) == 0) {
            return checkOld;
        }

        return checkNew;
    }

    // Cache các đường dẫn asset game chỉ đọc để tránh quét opendir/readdir lặp lại qua FUSE Android
    static std::mutex sAssetPathCacheMutex;
    static std::unordered_map<std::string, std::string> sAssetPathCache;
    const std::string cacheKey = rootDir + "|" + effectiveRelPath;
    {
        std::lock_guard<std::mutex> cacheLock(sAssetPathCacheMutex);
        auto it = sAssetPathCache.find(cacheKey);
        if (it != sAssetPathCache.end()) {
            return it->second;
        }
    }

    auto cacheResolvedAsset = [&](const std::string& resolved) -> const std::string& {
        std::lock_guard<std::mutex> cacheLock(sAssetPathCacheMutex);
        sAssetPathCache[cacheKey] = resolved;
        return resolved;
    };

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < effectiveRelPath.size()) {
        size_t end = effectiveRelPath.find('/', start);
        if (end == std::string::npos) end = effectiveRelPath.size();
        if (end > start) {
            std::string part = effectiveRelPath.substr(start, end - start);
            if (part != ".") {
                parts.push_back(part);
            }
        }
        start = end + 1;
    }

    std::string currentPath = rootDir;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& target = parts[i];
        std::string nextPath = currentPath + "/" + target;

        struct stat st;
        if (stat(nextPath.c_str(), &st) == 0) {
            currentPath = nextPath;
            continue;
        }

        DIR* dir = opendir(currentPath.c_str());
        if (!dir) {
            for (size_t j = i; j < parts.size(); ++j) {
                currentPath += "/" + parts[j];
            }
            return currentPath;
        }

        bool found = false;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcasecmp(entry->d_name, target.c_str()) == 0) {
                currentPath = currentPath + "/" + entry->d_name;
                found = true;
                break;
            }
        }
        closedir(dir);

        if (!found) {
            for (size_t j = i; j < parts.size(); ++j) {
                currentPath += "/" + parts[j];
            }
            break;
        }
    }

    struct stat st;
    if (stat(currentPath.c_str(), &st) == 0) {
        cacheResolvedAsset(currentPath);
        return currentPath;
    }

    // Nếu không tìm thấy, thử các fallback đường dẫn đặc trưng của Mages Engine:
    // 1. Nếu relPath bắt đầu bằng tiền tố như 'windata/', 'winweb/', 'wind3d11data/', 'win/':
    //    Thử bỏ tiền tố đó để tìm trực tiếp trong rootDir hoặc wind3d11data/
    static std::atomic<uint32_t> sFallbackLogCount{0};
    auto logFallbackHit = [&](const char* kind, const std::string& targetPath) {
        uint32_t cnt = ++sFallbackLogCount;
        if (cnt <= 40 || cnt % 200 == 1) {
            LOGI("VFS Fallback: '%s' -> tìm thấy tại %s: '%s'", relPath.c_str(), kind, targetPath.c_str());
        }
    };

    if (parts.size() > 1 && (parts[0].rfind("win", 0) == 0)) {
        std::string stripped;
        for (size_t k = 1; k < parts.size(); ++k) {
            if (!stripped.empty()) stripped += "/";
            stripped += parts[k];
        }

        // Thử tìm trong wind3d11data/
        std::string fallbackD3d = rootDir + "/wind3d11data/" + stripped;
        if (stat(fallbackD3d.c_str(), &st) == 0) {
            logFallbackHit("wind3d11data", fallbackD3d);
            cacheResolvedAsset(fallbackD3d);
            return fallbackD3d;
        }

        // Thử tìm trực tiếp ở rootDir
        std::string fallbackRoot = rootDir + "/" + stripped;
        if (stat(fallbackRoot.c_str(), &st) == 0) {
            logFallbackHit("root", fallbackRoot);
            cacheResolvedAsset(fallbackRoot);
            return fallbackRoot;
        }
    }

    // 2. Nếu relPath không có tiền tố wind3d11data, thử tìm trong wind3d11data/
    if (parts.size() >= 1 && parts[0] != "wind3d11data") {
        std::string fallbackD3d = rootDir + "/wind3d11data/" + relPath;
        if (stat(fallbackD3d.c_str(), &st) == 0) {
            logFallbackHit("wind3d11data", fallbackD3d);
            cacheResolvedAsset(fallbackD3d);
            return fallbackD3d;
        }
    }

    return currentPath;
}

static void ensureParentDirExists(const std::string& filePath) {
    size_t lastSlash = filePath.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
        std::string dirPath = filePath.substr(0, lastSlash);
        size_t pos = 1;
        while (pos < dirPath.size()) {
            pos = dirPath.find('/', pos);
            if (pos == std::string::npos) pos = dirPath.size();
            std::string sub = dirPath.substr(0, pos);
            mkdir(sub.c_str(), 0777);
            pos++;
        }
    }
}

void* WINAPI stub_CreateFileW(
    const char16_t* lpFileName,
    uint32_t dwDesiredAccess,
    uint32_t /* dwShareMode */,
    void* /* lpSecurityAttributes */,
    uint32_t dwCreationDisposition,
    uint32_t /* dwFlagsAndAttributes */,
    void* /* hTemplateFile */
) {
    if (!lpFileName || lpFileName[0] == 0) {
        stub_SetLastError(2);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::string relPath = normalizeWinPathW(lpFileName);
    if (relPath.empty() || relPath.find("(null") != std::string::npos) {
        LOGW("CreateFileW: Nhận đường dẫn không hợp lệ hoặc rỗng '%s' -> fallback mở /dev/null", relPath.c_str());
        int devNullFd = open("/dev/null", O_RDWR);
        if (devNullFd >= 0) {
            std::lock_guard<std::mutex> lock(gVfsMutex);
            uintptr_t handleId = ++gNextHandleId;
            gVfsHandles[handleId] = { devNullFd, "/dev/null", dwDesiredAccess };
            stub_SetLastError(0);
            return reinterpret_cast<void*>(handleId);
        }
        stub_SetLastError(2);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    bool isWrite = (dwDesiredAccess & 0x40000000) != 0;
    bool isRead = (dwDesiredAccess & 0x80000000) != 0;
    const bool isSaveRelated = (fullPath.find("/saves/") != std::string::npos || relPath.rfind("data_", 0) == 0 || relPath.rfind("meta_", 0) == 0);
    static uint32_t sOpenWLogCount = 0;
    const uint32_t openLogIdx = ++sOpenWLogCount;
    if (isWrite || isSaveRelated || openLogIdx <= 35 || openLogIdx % 250 == 1) {
        LOGI("CreateFileW: '%s' -> mapped: '%s', disp=%u, access=0x%X",
             relPath.c_str(), fullPath.c_str(), dwCreationDisposition, dwDesiredAccess);
    }

    int flags = 0;

    if (isWrite && isRead) flags |= O_RDWR;
    else if (isWrite) flags |= O_WRONLY;
    else flags |= O_RDONLY;

    switch (dwCreationDisposition) {
        case 1: // CREATE_NEW
            flags |= O_CREAT | O_EXCL;
            ensureParentDirExists(fullPath);
            break;
        case 2: // CREATE_ALWAYS
            flags |= O_CREAT | O_TRUNC;
            ensureParentDirExists(fullPath);
            break;
        case 3: // OPEN_EXISTING
            break;
        case 4: // OPEN_ALWAYS
            flags |= O_CREAT;
            ensureParentDirExists(fullPath);
            break;
        case 5: // TRUNCATE_EXISTING
            flags |= O_TRUNC;
            break;
    }

    int fd = open(fullPath.c_str(), flags, 0666);
    if (fd < 0) {
        LOGW("CreateFileW: Thất bại khi mở file: '%s' (errno=%d: %s)", fullPath.c_str(), errno, strerror(errno));
        stub_SetLastError(errno == ENOENT ? 2 : (errno == EACCES ? 5 : 2));
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    uintptr_t handleId = ++gNextHandleId;
    gVfsHandles[handleId] = { fd, fullPath, dwDesiredAccess };
    stub_SetLastError(0);
    if (isWrite || isSaveRelated || openLogIdx <= 35 || openLogIdx % 250 == 1) {
        LOGI("-> Mở file thành công! Handle=%p, fd=%d", reinterpret_cast<void*>(handleId), fd);
    }
    return reinterpret_cast<void*>(handleId);
}

void* WINAPI stub_CreateFileA(
    const char* lpFileName,
    uint32_t dwDesiredAccess,
    uint32_t /* dwShareMode */,
    void* /* lpSecurityAttributes */,
    uint32_t dwCreationDisposition,
    uint32_t /* dwFlagsAndAttributes */,
    void* /* hTemplateFile */
) {
    if (!lpFileName || lpFileName[0] == 0) {
        stub_SetLastError(2);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::string relPath = normalizeWinPathA(lpFileName);
    if (relPath.empty() || relPath.find("(null") != std::string::npos) {
        LOGW("CreateFileA: Nhận đường dẫn không hợp lệ hoặc rỗng '%s' -> fallback mở /dev/null", relPath.c_str());
        int devNullFd = open("/dev/null", O_RDWR);
        if (devNullFd >= 0) {
            std::lock_guard<std::mutex> lock(gVfsMutex);
            uintptr_t handleId = ++gNextHandleId;
            gVfsHandles[handleId] = { devNullFd, "/dev/null", dwDesiredAccess };
            stub_SetLastError(0);
            return reinterpret_cast<void*>(handleId);
        }
        stub_SetLastError(2);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    bool isWrite = (dwDesiredAccess & 0x40000000) != 0;
    bool isRead = (dwDesiredAccess & 0x80000000) != 0;
    const bool isSaveRelated = (fullPath.find("/saves/") != std::string::npos);
    static uint32_t sOpenALogCount = 0;
    const uint32_t openLogIdx = ++sOpenALogCount;
    if (isWrite || isSaveRelated || openLogIdx <= 20 || openLogIdx % 250 == 1) {
        LOGI("CreateFileA: '%s' -> mapped: '%s', disp=%u, access=0x%X",
             lpFileName, fullPath.c_str(), dwCreationDisposition, dwDesiredAccess);
    }

    int flags = 0;

    if (isWrite && isRead) flags |= O_RDWR;
    else if (isWrite) flags |= O_WRONLY;
    else flags |= O_RDONLY;

    switch (dwCreationDisposition) {
        case 1:
            flags |= O_CREAT | O_EXCL;
            ensureParentDirExists(fullPath);
            break;
        case 2:
            flags |= O_CREAT | O_TRUNC;
            ensureParentDirExists(fullPath);
            break;
        case 3:
            break;
        case 4:
            flags |= O_CREAT;
            ensureParentDirExists(fullPath);
            break;
        case 5:
            flags |= O_TRUNC;
            break;
    }

    int fd = open(fullPath.c_str(), flags, 0666);
    if (fd < 0) {
        LOGW("CreateFileA: Thất bại khi mở file: '%s' (errno=%d: %s)", fullPath.c_str(), errno, strerror(errno));
        stub_SetLastError(errno == ENOENT ? 2 : (errno == EACCES ? 5 : 2));
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    uintptr_t handleId = ++gNextHandleId;
    gVfsHandles[handleId] = { fd, fullPath, dwDesiredAccess };
    stub_SetLastError(0);
    if (isWrite || isSaveRelated || openLogIdx <= 35 || openLogIdx % 250 == 1) {
        LOGI("-> Mở file thành công! Handle=%p, fd=%d", reinterpret_cast<void*>(handleId), fd);
    }
    return reinterpret_cast<void*>(handleId);
}

int32_t WINAPI stub_ReadFile(
    void* hFile,
    void* lpBuffer,
    uint32_t nNumberOfBytesToRead,
    uint32_t* lpNumberOfBytesRead,
    void* lpOverlapped
) {
    auto* ov = reinterpret_cast<Win32Overlapped*>(lpOverlapped);
    if (!hFile || hFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1)) || !lpBuffer) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        if (ov) {
            ov->internal = 0xC0000008; // STATUS_INVALID_HANDLE
            ov->internalHigh = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
        }
        return 0;
    }

    int fd = -1;
    std::string filePath;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) {
            fd = it->second.fd;
            filePath = it->second.path;
        }
    }

    if (fd < 0) {
        LOGW("ReadFile: Handle không hợp lệ: %p", hFile);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        if (ov) {
            ov->internal = 0xC0000008; // STATUS_INVALID_HANDLE
            ov->internalHigh = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
        }
        return 0;
    }

    ssize_t bytesRead = 0;
    if (ov) {
        off64_t fileOffset = (static_cast<off64_t>(ov->offsetHigh) << 32) | static_cast<off64_t>(ov->offset);
        bytesRead = pread64(fd, lpBuffer, nNumberOfBytesToRead, fileOffset);
        if (bytesRead < 0) {
            LOGW("ReadFile (OVERLAPPED): Lỗi đọc fd=%d offset=%lld (errno=%d: %s)",
                 fd, static_cast<long long>(fileOffset), errno, strerror(errno));
            ov->internal = 0xC0000001; // STATUS_UNSUCCESSFUL
            ov->internalHigh = 0;
            if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
            return 0;
        }

        ov->internal = 0; // STATUS_SUCCESS
        ov->internalHigh = static_cast<uintptr_t>(bytesRead);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<uint32_t>(bytesRead);
        if (ov->hEvent) {
            stub_SetEvent(ov->hEvent);
        }
        static uint32_t sOvReadCount = 0;
        if (++sOvReadCount % 500 == 1) {
            LOGI("ReadFile (OVERLAPPED) #%u: Handle=%p, fd=%d, offset=%lld, bytesToRead=%u -> read=%zd, internalHigh=%lu",
                 sOvReadCount, hFile, fd, static_cast<long long>(fileOffset), nNumberOfBytesToRead, bytesRead, static_cast<unsigned long>(ov->internalHigh));
        }
        return 1;
    }

    bytesRead = read(fd, lpBuffer, nNumberOfBytesToRead);
    if (bytesRead < 0) {
        LOGW("ReadFile: Lỗi đọc fd=%d (errno=%d)", fd, errno);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return 0;
    }

    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<uint32_t>(bytesRead);
    static uint32_t sSyncReadCount = 0;
    if (++sSyncReadCount % 500 == 1) {
        LOGI("ReadFile #%u: Handle=%p, fd=%d, bytesToRead=%u -> read=%zd", sSyncReadCount, hFile, fd, nNumberOfBytesToRead, bytesRead);
    }
    return 1;
}

int32_t WINAPI stub_WriteFile(
    void* hFile,
    const void* lpBuffer,
    uint32_t nNumberOfBytesToWrite,
    uint32_t* lpNumberOfBytesWritten,
    void* lpOverlapped
) {
    auto* ov = reinterpret_cast<Win32Overlapped*>(lpOverlapped);
    if (!hFile || hFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1)) || !lpBuffer) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        if (ov) {
            ov->internal = 0xC0000008; // STATUS_INVALID_HANDLE
            ov->internalHigh = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
        }
        return 0;
    }

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }

    if (fd < 0) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        if (ov) {
            ov->internal = 0xC0000008; // STATUS_INVALID_HANDLE
            ov->internalHigh = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
        }
        return 0;
    }

    ssize_t bytesWritten = 0;
    if (ov) {
        off64_t fileOffset = (static_cast<off64_t>(ov->offsetHigh) << 32) | static_cast<off64_t>(ov->offset);
        bytesWritten = pwrite64(fd, lpBuffer, nNumberOfBytesToWrite, fileOffset);
        if (bytesWritten < 0) {
            ov->internal = 0xC0000001; // STATUS_UNSUCCESSFUL
            ov->internalHigh = 0;
            if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
            if (ov->hEvent) stub_SetEvent(ov->hEvent);
            return 0;
        }

        ov->internal = 0; // STATUS_SUCCESS
        ov->internalHigh = static_cast<uintptr_t>(bytesWritten);
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<uint32_t>(bytesWritten);
        if (ov->hEvent) {
            stub_SetEvent(ov->hEvent);
        }
        static uint32_t sOvWriteCount = 0;
        if (++sOvWriteCount % 500 == 1) {
            LOGI("WriteFile (OVERLAPPED) #%u: Handle=%p, fd=%d, offset=%lld, bytesToWrite=%u -> written=%zd",
                 sOvWriteCount, hFile, fd, static_cast<long long>(fileOffset), nNumberOfBytesToWrite, bytesWritten);
        }
        return 1;
    }

    bytesWritten = write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (bytesWritten < 0) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return 0;
    }

    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<uint32_t>(bytesWritten);
    static uint32_t sSyncWriteCount = 0;
    if (++sSyncWriteCount % 500 == 1) {
        LOGI("WriteFile #%u: Handle=%p, fd=%d, bytesToWrite=%u -> written=%zd", sSyncWriteCount, hFile, fd, nNumberOfBytesToWrite, bytesWritten);
    }
    return 1;
}

uint32_t WINAPI stub_SetFilePointer(
    void* hFile,
    int32_t lDistanceToMove,
    int32_t* lpDistanceToMoveHigh,
    uint32_t dwMoveMethod
) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd < 0) return 0xFFFFFFFF;

    int64_t offset = lDistanceToMove;
    if (lpDistanceToMoveHigh) {
        offset |= (static_cast<int64_t>(*lpDistanceToMoveHigh) << 32);
    }

    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;

    off64_t res = lseek64(fd, offset, whence);
    if (res == -1) return 0xFFFFFFFF;

    if (lpDistanceToMoveHigh) {
        *lpDistanceToMoveHigh = static_cast<int32_t>(res >> 32);
    }
    return static_cast<uint32_t>(res & 0xFFFFFFFF);
}

int32_t WINAPI stub_SetFilePointerEx(
    void* hFile,
    int64_t liDistanceToMove,
    int64_t* lpNewFilePointer,
    uint32_t dwMoveMethod
) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd < 0) return 0;

    int whence = SEEK_SET;
    if (dwMoveMethod == 1) whence = SEEK_CUR;
    else if (dwMoveMethod == 2) whence = SEEK_END;

    off64_t res = lseek64(fd, liDistanceToMove, whence);
    if (res == -1) return 0;

    if (lpNewFilePointer) *lpNewFilePointer = res;
    return 1;
}

uint32_t WINAPI stub_GetFileSize(void* hFile, uint32_t* lpFileSizeHigh) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd < 0) return 0xFFFFFFFF;

    struct stat64 st;
    if (fstat64(fd, &st) != 0) return 0xFFFFFFFF;

    if (lpFileSizeHigh) *lpFileSizeHigh = static_cast<uint32_t>(st.st_size >> 32);
    return static_cast<uint32_t>(st.st_size & 0xFFFFFFFF);
}

int32_t WINAPI stub_GetFileSizeEx(void* hFile, int64_t* lpFileSize) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd < 0 || !lpFileSize) return 0;

    struct stat64 st;
    if (fstat64(fd, &st) != 0) return 0;

    *lpFileSize = st.st_size;
    return 1;
}

int32_t WINAPI stub_GetFileInformationByHandle(void* hFile, void* lpFileInformation) {
    if (!hFile || !lpFileInformation) return 0;

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd < 0) return 0;

    struct stat64 st;
    if (fstat64(fd, &st) != 0) return 0;

    auto* info = reinterpret_cast<Win32ByHandleFileInformation*>(lpFileInformation);
    memset(info, 0, sizeof(Win32ByHandleFileInformation));
    info->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    info->nFileSizeHigh = static_cast<uint32_t>(st.st_size >> 32);
    info->nFileSizeLow = static_cast<uint32_t>(st.st_size & 0xFFFFFFFF);
    info->nNumberOfLinks = 1;
    return 1;
}

int32_t WINAPI stub_GetOverlappedResult(
    void* /* hFile */,
    void* lpOverlapped,
    uint32_t* lpNumberOfBytesTransferred,
    int32_t bWait
) {
    if (!lpOverlapped) return 0;
    auto* ov = reinterpret_cast<Win32Overlapped*>(lpOverlapped);

    if (bWait && ov->hEvent) {
        stub_WaitForSingleObject(ov->hEvent, WIN32_INFINITE);
    }

    if (lpNumberOfBytesTransferred) {
        *lpNumberOfBytesTransferred = static_cast<uint32_t>(ov->internalHigh);
    }

    // internal == 0 tương ứng với STATUS_SUCCESS
    return (ov->internal == 0) ? 1 : 0;
}

int32_t WINAPI stub_CancelIo(void* /* hFile */) {
    return 1;
}

int32_t WINAPI stub_CancelIoEx(void* /* hFile */, void* /* lpOverlapped */) {
    return 1;
}

int32_t WINAPI stub_CloseHandle(void* hObject) {
    if (!hObject || hObject == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) {
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(gSyncMutex);
        auto sit = gSyncMap.find(reinterpret_cast<uintptr_t>(hObject));
        if (sit != gSyncMap.end()) {
            gSyncMap.erase(sit);
            return 1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(gThreadMutex);
        auto tit = gThreadMap.find(reinterpret_cast<uintptr_t>(hObject));
        if (tit != gThreadMap.end()) {
            LOGI("CloseHandle: Đóng thread handle %p (TID=%u, finished=%d)",
                 hObject, tit->second->threadId, tit->second->finished ? 1 : 0);
            gThreadMap.erase(tit);
            return 1;
        }
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hObject));
    if (it != gVfsHandles.end()) {
        const bool isWriteHandle = (it->second.accessMode & 0x40000000) != 0;
        const bool isSavePath = (it->second.path.find("/saves/") != std::string::npos);
        static uint32_t sCloseFileLogCount = 0;
        const uint32_t closeIdx = ++sCloseFileLogCount;
        if (isWriteHandle || isSavePath || closeIdx <= 35 || closeIdx % 250 == 1) {
            LOGI("CloseHandle: file %p (fd=%d, path='%s')", hObject, it->second.fd, it->second.path.c_str());
        }
        // Neu file co quyen ghi (GENERIC_WRITE: 0x40000000), dong bo du lieu xuong disk de bao ve toan ven file save
        if (isWriteHandle) {
            fsync(it->second.fd);
        }
        close(it->second.fd);
        gVfsHandles.erase(it);
        return 1;
    }

    auto fit = gVfsFindStates.find(reinterpret_cast<uintptr_t>(hObject));
    if (fit != gVfsFindStates.end()) {
        LOGI("CloseHandle: find handle %p", hObject);
        gVfsFindStates.erase(fit);
        return 1;
    }

    return 1;
}

uint32_t WINAPI stub_GetFileAttributesW(const char16_t* lpFileName) {
    if (!lpFileName) return 0xFFFFFFFF;

    std::string relPath = normalizeWinPathW(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    struct stat64 st;
    if (stat64(fullPath.c_str(), &st) != 0) {
        if (relPath == "windata" || relPath == "winweb" || relPath == "wind3d11data") {
            return 0x10; // FILE_ATTRIBUTE_DIRECTORY
        }
        LOGI("GetFileAttributesW: Không tìm thấy '%s'", fullPath.c_str());
        return 0xFFFFFFFF;
    }

    if (S_ISDIR(st.st_mode)) return 0x10; // FILE_ATTRIBUTE_DIRECTORY
    return 0x80; // FILE_ATTRIBUTE_NORMAL
}

uint32_t WINAPI stub_GetFileAttributesA(const char* lpFileName) {
    if (!lpFileName) return 0xFFFFFFFF;

    std::string relPath = normalizeWinPathA(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    struct stat64 st;
    if (stat64(fullPath.c_str(), &st) != 0) {
        if (relPath == "windata" || relPath == "winweb" || relPath == "wind3d11data") {
            return 0x10; // FILE_ATTRIBUTE_DIRECTORY
        }
        return 0xFFFFFFFF;
    }

    if (S_ISDIR(st.st_mode)) return 0x10;
    return 0x80;
}

void* WINAPI stub_FindFirstFileW(const char16_t* lpFileName, void* lpFindFileData) {
    if (!lpFileName || !lpFindFileData) {
        stub_SetLastError(87); // ERROR_INVALID_PARAMETER
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::string normPath = normalizeWinPathW(lpFileName);
    size_t lastSlash = normPath.rfind('/');
    std::string dirPart = (lastSlash != std::string::npos) ? normPath.substr(0, lastSlash) : "";
    std::string patPart = (lastSlash != std::string::npos) ? normPath.substr(lastSlash + 1) : normPath;

    if (dirPart.empty()) {
        std::lock_guard<std::mutex> lock(gCurrentDirMutex);
        if (!gCurrentVfsDirectory.empty()) {
            dirPart = gCurrentVfsDirectory;
        } else if (patPart.rfind("data_", 0) == 0 || patPart.rfind("meta_", 0) == 0) {
            dirPart = "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930";
        }
    }

    std::vector<std::string> candidateDirs;

    // Phân tích định tuyến thư mục theo Save Data hoặc Data thông thường
    if (dirPart.find("<Documents>") != std::string::npos || dirPart.rfind("saves", 0) == 0) {
        std::string sub;
        size_t docPos = dirPart.find("<Documents>");
        if (docPos != std::string::npos) {
            sub = dirPart.substr(docPos + 11);
        } else {
            sub = dirPart.substr(5);
        }
        while (!sub.empty() && (sub[0] == '/' || sub[0] == '\\')) sub.erase(0, 1);

        std::string baseNew = gVfsDataDir + "/saves";
        std::string baseOld = gVfsDataDir + "/<Documents>";
        std::string dirNew = resolveCaseInsensitiveSubPath(baseNew, sub);
        std::string dirOld = resolveCaseInsensitiveSubPath(baseOld, sub);

        struct stat st;
        if (stat(dirNew.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            candidateDirs.push_back(dirNew);
        }
        if (stat(dirOld.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (dirOld != dirNew) {
                candidateDirs.push_back(dirOld);
            }
        }
        if (candidateDirs.empty()) {
            candidateDirs.push_back(dirNew);
            candidateDirs.push_back(dirOld);
        }
    } else if (!dirPart.empty() && dirPart.rfind("win", 0) == 0) {
        std::string primaryDir = resolveCaseInsensitivePath(gVfsDataDir, dirPart);
        candidateDirs.push_back(primaryDir);
        std::string d3dDir = gVfsDataDir + "/wind3d11data";
        if (d3dDir != primaryDir) candidateDirs.push_back(d3dDir);
        if (gVfsDataDir != primaryDir) candidateDirs.push_back(gVfsDataDir);
    } else {
        std::string primaryDir = resolveCaseInsensitivePath(gVfsDataDir, dirPart);
        candidateDirs.push_back(primaryDir);
    }

    // Quét Union Find trên tất cả các candidateDirs để gom đầy đủ file
    std::vector<VfsFindEntry> entries;
    std::unordered_map<std::string, size_t> seenFiles;

    for (const std::string& fullDirPath : candidateDirs) {
        DIR* dir = opendir(fullDirPath.c_str());
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            bool match = (patPart == "*" || patPart == "*.*") ||
                         (fnmatch(patPart.c_str(), entry->d_name, FNM_CASEFOLD) == 0);

            if (match) {
                std::string itemFullPath = fullDirPath + "/" + entry->d_name;
                struct stat64 st;
                uint32_t attr = 0x80;
                uint64_t fileSize = 0;
                Win32FileTime ftCreate = {0, 0};
                Win32FileTime ftAccess = {0, 0};
                Win32FileTime ftWrite = {0, 0};
                time_t mtimeSec = 0;

                if (stat64(itemFullPath.c_str(), &st) == 0) {
                    attr = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
                    fileSize = static_cast<uint64_t>(st.st_size);
                    unixTimeToWin32FileTime(st.st_ctime, 0, &ftCreate);
                    unixTimeToWin32FileTime(st.st_atime, 0, &ftAccess);
                    unixTimeToWin32FileTime(st.st_mtime, 0, &ftWrite);
                    mtimeSec = st.st_mtime;
                }

                std::string lowerName = entry->d_name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

                auto itSeen = seenFiles.find(lowerName);
                if (itSeen == seenFiles.end()) {
                    VfsFindEntry fe;
                    fe.fileName = entry->d_name;
                    fe.attributes = attr;
                    fe.fileSizeHigh = static_cast<uint32_t>(fileSize >> 32);
                    fe.fileSizeLow = static_cast<uint32_t>(fileSize & 0xFFFFFFFF);
                    fe.ftCreationTime = ftCreate;
                    fe.ftLastAccessTime = ftAccess;
                    fe.ftLastWriteTime = ftWrite;
                    fe.mtimeSec = mtimeSec;

                    seenFiles[lowerName] = entries.size();
                    entries.push_back(std::move(fe));
                } else {
                    // Nếu file đã có trong danh sách, cập nhật bản ghi mới hơn theo mtime
                    size_t idx = itSeen->second;
                    if (mtimeSec > entries[idx].mtimeSec) {
                        entries[idx].fileName = entry->d_name;
                        entries[idx].attributes = attr;
                        entries[idx].fileSizeHigh = static_cast<uint32_t>(fileSize >> 32);
                        entries[idx].fileSizeLow = static_cast<uint32_t>(fileSize & 0xFFFFFFFF);
                        entries[idx].ftCreationTime = ftCreate;
                        entries[idx].ftLastAccessTime = ftAccess;
                        entries[idx].ftLastWriteTime = ftWrite;
                        entries[idx].mtimeSec = mtimeSec;
                    }
                }
            }
        }
        closedir(dir);
    }

    if (entries.empty()) {
        stub_SetLastError(2); // ERROR_FILE_NOT_FOUND
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    auto* findData = reinterpret_cast<Win32FindDataW*>(lpFindFileData);
    memset(findData, 0, sizeof(Win32FindDataW));
    findData->dwFileAttributes = entries[0].attributes;
    findData->ftCreationTime = entries[0].ftCreationTime;
    findData->ftLastAccessTime = entries[0].ftLastAccessTime;
    findData->ftLastWriteTime = entries[0].ftLastWriteTime;
    findData->nFileSizeHigh = entries[0].fileSizeHigh;
    findData->nFileSizeLow = entries[0].fileSizeLow;
    copyUtf8ToUtf16(entries[0].fileName, findData->cFileName, 260);

    std::lock_guard<std::mutex> lock(gVfsMutex);
    uintptr_t findId = ++gNextHandleId;
    gVfsFindStates[findId] = { std::move(entries), 1 };
    stub_SetLastError(0);
    return reinterpret_cast<void*>(findId);
}

void* WINAPI stub_FindFirstFileExW(
    const char16_t* lpFileName,
    int32_t,
    void* lpFindFileData,
    int32_t,
    void*,
    uint32_t
) {
    return stub_FindFirstFileW(lpFileName, lpFindFileData);
}

int32_t WINAPI stub_FindNextFileW(void* hFindFile, void* lpFindFileData) {
    if (!hFindFile || hFindFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1)) || !lpFindFileData) {
        stub_SetLastError(6); // ERROR_INVALID_HANDLE
        return 0;
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    auto it = gVfsFindStates.find(reinterpret_cast<uintptr_t>(hFindFile));
    if (it == gVfsFindStates.end()) {
        stub_SetLastError(6); // ERROR_INVALID_HANDLE
        return 0;
    }

    if (it->second.currentIndex >= it->second.entries.size()) {
        stub_SetLastError(18); // ERROR_NO_MORE_FILES
        return 0;
    }

    const auto& entry = it->second.entries[it->second.currentIndex++];
    auto* findData = reinterpret_cast<Win32FindDataW*>(lpFindFileData);
    memset(findData, 0, sizeof(Win32FindDataW));
    findData->dwFileAttributes = entry.attributes;
    findData->ftCreationTime = entry.ftCreationTime;
    findData->ftLastAccessTime = entry.ftLastAccessTime;
    findData->ftLastWriteTime = entry.ftLastWriteTime;
    findData->nFileSizeHigh = entry.fileSizeHigh;
    findData->nFileSizeLow = entry.fileSizeLow;
    copyUtf8ToUtf16(entry.fileName, findData->cFileName, 260);

    stub_SetLastError(0);
    return 1;
}

int32_t WINAPI stub_FindClose(void* hFindFile) {
    if (!hFindFile || hFindFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) return 1;
    std::lock_guard<std::mutex> lock(gVfsMutex);
    gVfsFindStates.erase(reinterpret_cast<uintptr_t>(hFindFile));
    return 1;
}

void* WINAPI stub_FindFirstFileA(const char* lpFileName, void* lpFindFileData) {
    if (!lpFileName || !lpFindFileData) {
        stub_SetLastError(87);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
    char16_t wBuf[512] = {0};
    copyUtf8ToUtf16(lpFileName, wBuf, 512);

    Win32FindDataW findDataW;
    void* hFind = stub_FindFirstFileW(wBuf, &findDataW);
    if (hFind == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) {
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    auto* findDataA = reinterpret_cast<Win32FindDataA*>(lpFindFileData);
    memset(findDataA, 0, sizeof(Win32FindDataA));
    findDataA->dwFileAttributes = findDataW.dwFileAttributes;
    findDataA->ftCreationTime = findDataW.ftCreationTime;
    findDataA->ftLastAccessTime = findDataW.ftLastAccessTime;
    findDataA->ftLastWriteTime = findDataW.ftLastWriteTime;
    findDataA->nFileSizeHigh = findDataW.nFileSizeHigh;
    findDataA->nFileSizeLow = findDataW.nFileSizeLow;
    utf16ToUtf8(findDataW.cFileName, utf16Len(findDataW.cFileName), findDataA->cFileName, 260);

    return hFind;
}

int32_t WINAPI stub_FindNextFileA(void* hFindFile, void* lpFindFileData) {
    if (!hFindFile || !lpFindFileData) return 0;
    Win32FindDataW findDataW;
    int32_t ret = stub_FindNextFileW(hFindFile, &findDataW);
    if (!ret) return 0;

    auto* findDataA = reinterpret_cast<Win32FindDataA*>(lpFindFileData);
    memset(findDataA, 0, sizeof(Win32FindDataA));
    findDataA->dwFileAttributes = findDataW.dwFileAttributes;
    findDataA->ftCreationTime = findDataW.ftCreationTime;
    findDataA->ftLastAccessTime = findDataW.ftLastAccessTime;
    findDataA->ftLastWriteTime = findDataW.ftLastWriteTime;
    findDataA->nFileSizeHigh = findDataW.nFileSizeHigh;
    findDataA->nFileSizeLow = findDataW.nFileSizeLow;
    utf16ToUtf8(findDataW.cFileName, utf16Len(findDataW.cFileName), findDataA->cFileName, 260);

    return 1;
}

int32_t WINAPI stub_FlushFileBuffers(void* hFile) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd >= 0) fsync(fd);
    return 1;
}

int32_t WINAPI stub_SetEndOfFile(void* hFile) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }
    if (fd >= 0) {
        off64_t curr = lseek64(fd, 0, SEEK_CUR);
        if (curr >= 0) ftruncate64(fd, curr);
    }
    return 1;
}

int32_t WINAPI stub_DeleteFileW(const char16_t* lpFileName) {
    if (!lpFileName) return 0;
    std::string relPath = normalizeWinPathW(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);
    return unlink(fullPath.c_str()) == 0 ? 1 : 0;
}

int32_t WINAPI stub_DeleteFileA(const char* lpFileName) {
    if (!lpFileName) return 0;
    std::string relPath = normalizeWinPathA(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);
    return unlink(fullPath.c_str()) == 0 ? 1 : 0;
}

int32_t WINAPI stub_GetCurrentDirectoryW(uint32_t nBufferLength, char16_t* lpBuffer) {
    std::lock_guard<std::mutex> lock(gCurrentDirMutex);
    std::string cur = gCurrentVfsDirectory.empty() ? "C:\\" : ("C:\\" + gCurrentVfsDirectory);
    for (char& c : cur) {
        if (c == '/') c = '\\';
    }
    LOGI("GetCurrentDirectoryW: -> '%s'", cur.c_str());
    if (lpBuffer && nBufferLength > cur.size()) {
        copyUtf8ToUtf16(cur.c_str(), lpBuffer, nBufferLength);
        return static_cast<int32_t>(cur.size());
    }
    return static_cast<int32_t>(cur.size() + 1);
}

int32_t WINAPI stub_SetCurrentDirectoryW(const char16_t* lpPath) {
    if (!lpPath) return 0;
    std::string s = normalizeWinPathW(lpPath);
    LOGI("SetCurrentDirectoryW: '%s'", s.c_str());
    std::lock_guard<std::mutex> lock(gCurrentDirMutex);
    gCurrentVfsDirectory = s;
    return 1;
}

int32_t WINAPI stub_GetCurrentDirectoryA(uint32_t nBufferLength, char* lpBuffer) {
    std::lock_guard<std::mutex> lock(gCurrentDirMutex);
    std::string cur = gCurrentVfsDirectory.empty() ? "C:\\" : ("C:\\" + gCurrentVfsDirectory);
    for (char& c : cur) {
        if (c == '/') c = '\\';
    }
    LOGI("GetCurrentDirectoryA: -> '%s'", cur.c_str());
    if (lpBuffer && nBufferLength > cur.size()) {
        strncpy(lpBuffer, cur.c_str(), nBufferLength);
        return static_cast<int32_t>(cur.size());
    }
    return static_cast<int32_t>(cur.size() + 1);
}

int32_t WINAPI stub_SetCurrentDirectoryA(const char* lpPath) {
    if (!lpPath) return 0;
    std::string s = normalizeWinPathA(lpPath);
    LOGI("SetCurrentDirectoryA: '%s'", s.c_str());
    std::lock_guard<std::mutex> lock(gCurrentDirMutex);
    gCurrentVfsDirectory = s;
    return 1;
}
uint32_t WINAPI stub_GetModuleFileNameA(void*, char* lpFilename, uint32_t nSize) {
    if (lpFilename && nSize > 14) {
        strcpy(lpFilename, "sgre_steam.exe");
        return 14;
    }
    return 0;
}
uint32_t WINAPI stub_GetModuleFileNameW(void*, char16_t* lpFilename, uint32_t nSize) {
    if (lpFilename && nSize > 14) {
        copyUtf8ToUtf16("sgre_steam.exe", lpFilename, nSize);
        return 14;
    }
    return 0;
}

uint32_t WINAPI stub_GetCurrentProcessId() { return static_cast<uint32_t>(getpid()); }
uint32_t WINAPI stub_GetCurrentThreadId() {
#if defined(__ANDROID__) || defined(__linux__)
    return static_cast<uint32_t>(syscall(SYS_gettid));
#else
    return static_cast<uint32_t>(pthread_self());
#endif
}
void* WINAPI stub_GetCurrentProcess() { return reinterpret_cast<void*>(-1); }

void* WINAPI stub_GetModuleHandleA(const char* lpModuleName) {
    if (!lpModuleName || lpModuleName[0] == '\0') {
        return gLoadedImageBase ? gLoadedImageBase : DUMMY_MODULE;
    }
    return DUMMY_MODULE;
}

void* WINAPI stub_GetModuleHandleW(const char16_t* lpModuleName) {
    if (!lpModuleName || lpModuleName[0] == u'\0') {
        return gLoadedImageBase ? gLoadedImageBase : DUMMY_MODULE;
    }
    return DUMMY_MODULE;
}

void* WINAPI stub_GetModuleHandleExW(uint32_t, const char16_t* lpModuleName, void** phModule) {
    void* hMod = stub_GetModuleHandleW(lpModuleName);
    if (phModule) *phModule = hMod;
    return hMod;
}

void* WINAPI stub_LoadLibraryA(const char* lpLibFileName) {
    if (lpLibFileName) {
        LOGI("LoadLibraryA: %s", lpLibFileName);
        // Nếu game cố nạp libjack hoặc thư viện âm thanh ngoài, trả về nullptr để miniaudio fallback về driver audio nội bộ/WaveOut
        if (strstr(lpLibFileName, "jack") != nullptr || strstr(lpLibFileName, "JACK") != nullptr) {
            LOGW("LoadLibraryA: Từ chối '%s' -> trả về nullptr để game fallback", lpLibFileName);
            return nullptr;
        }
    }
    return DUMMY_MODULE;
}

void* WINAPI stub_LoadLibraryExW(const char16_t* lpLibFileName, void*, uint32_t) {
    if (lpLibFileName) {
        std::string libName = normalizeWinPathW(lpLibFileName);
        LOGI("LoadLibraryExW: %s", libName.c_str());
        if (libName.find("jack") != std::string::npos || libName.find("JACK") != std::string::npos) {
            LOGW("LoadLibraryExW: Từ chối '%s' -> trả về nullptr để game fallback", libName.c_str());
            return nullptr;
        }
    }
    return DUMMY_MODULE;
}
int32_t WINAPI stub_FreeLibrary(void*) { return 1; }

// SHELL32 API Implementation
int32_t WINAPI stub_SHGetSpecialFolderPathW(void*, char16_t* pszPath, int32_t csidl, int32_t fCreate) {
    if (!pszPath) return 0;
    // Map CSIDL_PERSONAL (0x0005) hoặc folder lưu game vào C:\saves
    copyUtf8ToUtf16("C:\\saves", pszPath, 260);

    if (fCreate) {
        std::string savesDir = gVfsDataDir + "/saves";
        mkdir(savesDir.c_str(), 0777);
    }
    LOGI("SHGetSpecialFolderPathW: csidl=0x%X -> 'C:\\saves'", csidl);
    return 1; // TRUE
}

int32_t WINAPI stub_SHCreateDirectoryExW(void*, const char16_t* pszPath, void*) {
    if (!pszPath) return 0;
    std::string relPath = normalizeWinPathW(pszPath);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);
    ensureParentDirExists(fullPath + "/placeholder");
    mkdir(fullPath.c_str(), 0777);
    LOGI("SHCreateDirectoryExW: tạo thành công thư mục '%s'", fullPath.c_str());
    return 0; // ERROR_SUCCESS
}

int32_t WINAPI stub_SHGetKnownFolderPath(const void* /* rfid */, uint32_t dwFlags, void* /* hToken */, char16_t** ppszPath) {
    if (!ppszPath) return static_cast<int32_t>(0x80070057); // E_INVALIDARG
    std::string path = "C:\\saves";
    size_t len = path.size() + 1;
    char16_t* wstr = static_cast<char16_t*>(malloc(len * sizeof(char16_t)));
    if (!wstr) return static_cast<int32_t>(0x8007000E); // E_OUTOFMEMORY
    copyUtf8ToUtf16(path.c_str(), wstr, len);
    *ppszPath = wstr;
    if (dwFlags & 0x8000 /* KF_FLAG_CREATE */) {
        std::string fullSavesDir = gVfsDataDir + "/saves";
        mkdir(fullSavesDir.c_str(), 0777);
    }
    LOGI("SHGetKnownFolderPath: allocated 'C:\\saves' -> %p", wstr);
    return 0; // S_OK
}

void* WINAPI stub_CoTaskMemAlloc(size_t cb) {
    return malloc(cb);
}

void* WINAPI stub_ShellExecuteW(void*, const char16_t*, const char16_t*, const char16_t*, const char16_t*, int32_t) {
    return reinterpret_cast<void*>(33); // Thành công (> 32)
}

void WINAPI stub_DragAcceptFiles(void*, int32_t) {}

int32_t WINAPI stub_MessageBoxA(void*, const char* lpText, const char* lpCaption, uint32_t) {
    LOGW("MessageBoxA: [%s] %s", lpCaption ? lpCaption : "", lpText ? lpText : "");
    return 1; // IDOK
}

int32_t WINAPI stub_MessageBoxW(void*, const char16_t* lpText, const char16_t* lpCaption, uint32_t) {
    std::string text = normalizeWinPathW(lpText);
    std::string caption = normalizeWinPathW(lpCaption);
    LOGW("MessageBoxW: [%s] %s", caption.c_str(), text.c_str());
    return 1; // IDOK
}

void* WINAPI stub_GetProcAddress(void*, const char* lpProcName) {
    if (!lpProcName) return nullptr;
    LOGI("GetProcAddress dynamic query: %s", lpProcName);
    return Win32Bridge_ResolveSymbol(nullptr, lpProcName);
}

static const auto sBootTime = std::chrono::steady_clock::now();

extern "C" void Win32Bridge_OnFramePresented() {
    // Hook đồng bộ Present1
}

uint32_t WINAPI stub_GetTickCount() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sBootTime).count();
    return static_cast<uint32_t>(ms);
}

uint64_t WINAPI stub_GetTickCount64() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - sBootTime).count();
    return static_cast<uint64_t>(ms);
}

int32_t WINAPI stub_QueryPerformanceCounter(int64_t* lpPerformanceCount) {
    if (lpPerformanceCount) {
        auto now = std::chrono::steady_clock::now();
        // 1 tick = 100ns -> 10,000,000 counts per second (10 MHz chuẩn Windows QPC)
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - sBootTime).count();
        *lpPerformanceCount = static_cast<int64_t>(ns / 100LL);
    }
    return 1;
}

int32_t WINAPI stub_QueryPerformanceFrequency(int64_t* lpFrequency) {
    if (lpFrequency) {
        *lpFrequency = 10000000LL; // 10 MHz (chuẩn Windows x64: 10,000,000 ticks/sec)
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

constexpr uint64_t kWin32EpochDiffSeconds = 11644473600ULL;
constexpr uint64_t kWin32TicksPerSecond = 10000000ULL;
constexpr uint64_t kWin32TicksPerMs = 10000ULL;

struct Win32SystemTime {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek;
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds;
};

static int64_t daysFromCivilUtc(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? static_cast<uint32_t>(-3) : 9u)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int32_t WINAPI stub_SystemTimeToFileTime(const void* lpSystemTime, void* lpFileTime) {
    if (!lpSystemTime || !lpFileTime) return 0;
    const auto* st = reinterpret_cast<const Win32SystemTime*>(lpSystemTime);
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31) return 0;

    int64_t days = daysFromCivilUtc(st->wYear, st->wMonth, st->wDay);
    int64_t unixSec = days * 86400LL +
                      static_cast<int64_t>(st->wHour) * 3600LL +
                      static_cast<int64_t>(st->wMinute) * 60LL +
                      static_cast<int64_t>(st->wSecond);
    int64_t ticksFromEpoch = (unixSec + static_cast<int64_t>(kWin32EpochDiffSeconds)) *
                             static_cast<int64_t>(kWin32TicksPerSecond) +
                             static_cast<int64_t>(st->wMilliseconds) * static_cast<int64_t>(kWin32TicksPerMs);
    if (ticksFromEpoch < 0) return 0;
    *reinterpret_cast<uint64_t*>(lpFileTime) = static_cast<uint64_t>(ticksFromEpoch);
    return 1;
}

int32_t WINAPI stub_FileTimeToSystemTime(const void* lpFileTime, void* lpSystemTime) {
    if (!lpFileTime || !lpSystemTime) return 0;
    uint64_t ticks = *reinterpret_cast<const uint64_t*>(lpFileTime);
    uint64_t totalSec = ticks / kWin32TicksPerSecond;
    uint16_t ms = static_cast<uint16_t>((ticks % kWin32TicksPerSecond) / kWin32TicksPerMs);
    time_t unixSec = static_cast<time_t>(static_cast<int64_t>(totalSec) - static_cast<int64_t>(kWin32EpochDiffSeconds));

    struct tm utcTm{};
    if (!gmtime_r(&unixSec, &utcTm)) return 0;

    auto* st = reinterpret_cast<Win32SystemTime*>(lpSystemTime);
    st->wYear = static_cast<uint16_t>(utcTm.tm_year + 1900);
    st->wMonth = static_cast<uint16_t>(utcTm.tm_mon + 1);
    st->wDayOfWeek = static_cast<uint16_t>(utcTm.tm_wday);
    st->wDay = static_cast<uint16_t>(utcTm.tm_mday);
    st->wHour = static_cast<uint16_t>(utcTm.tm_hour);
    st->wMinute = static_cast<uint16_t>(utcTm.tm_min);
    st->wSecond = static_cast<uint16_t>(utcTm.tm_sec);
    st->wMilliseconds = ms;
    return 1;
}

int32_t WINAPI stub_FileTimeToLocalFileTime(const void* lpFileTime, void* lpLocalFileTime) {
    if (!lpFileTime || !lpLocalFileTime) return 0;
    uint64_t ticks = *reinterpret_cast<const uint64_t*>(lpFileTime);
    time_t unixSec = static_cast<time_t>(
        static_cast<int64_t>(ticks / kWin32TicksPerSecond) - static_cast<int64_t>(kWin32EpochDiffSeconds));
    struct tm localTm{};
    int64_t offsetTicks = 0;
    if (localtime_r(&unixSec, &localTm)) {
        offsetTicks = static_cast<int64_t>(localTm.tm_gmtoff) * static_cast<int64_t>(kWin32TicksPerSecond);
    }
    int64_t localTicks = static_cast<int64_t>(ticks) + offsetTicks;
    *reinterpret_cast<uint64_t*>(lpLocalFileTime) = localTicks > 0 ? static_cast<uint64_t>(localTicks) : 0ULL;
    return 1;
}

uint32_t WINAPI stub_GetLastError() { return gLastErrorCode; }
void WINAPI stub_SetLastError(uint32_t err) { gLastErrorCode = err; }

// Phân hệ đồng bộ Win32 CRITICAL_SECTION dựa trên std::recursive_mutex (hỗ trợ khóa lồng nhau trên cùng thread)
struct Win32CSEntry {
    std::recursive_mutex rmtx;
};

static std::mutex gCSTableMutex;
static std::unordered_map<void*, std::shared_ptr<Win32CSEntry>> gCSTable;

static std::shared_ptr<Win32CSEntry> getOrCreateCS(void* lpCriticalSection) {
    if (!lpCriticalSection) return nullptr;
    std::lock_guard<std::mutex> lock(gCSTableMutex);
    auto it = gCSTable.find(lpCriticalSection);
    if (it != gCSTable.end()) {
        return it->second;
    }
    auto entry = std::make_shared<Win32CSEntry>();
    gCSTable[lpCriticalSection] = entry;
    return entry;
}

void WINAPI stub_InitializeCriticalSection(void* lpCriticalSection) {
    getOrCreateCS(lpCriticalSection);
}

int32_t WINAPI stub_InitializeCriticalSectionAndSpinCount(void* lpCriticalSection, uint32_t) {
    getOrCreateCS(lpCriticalSection);
    return 1;
}

int32_t WINAPI stub_InitializeCriticalSectionEx(void* lpCriticalSection, uint32_t, uint32_t) {
    getOrCreateCS(lpCriticalSection);
    return 1;
}

void WINAPI stub_EnterCriticalSection(void* lpCriticalSection) {
    auto entry = getOrCreateCS(lpCriticalSection);
    if (entry) {
        entry->rmtx.lock();
    }
}

int32_t WINAPI stub_TryEnterCriticalSection(void* lpCriticalSection) {
    auto entry = getOrCreateCS(lpCriticalSection);
    return (entry && entry->rmtx.try_lock()) ? 1 : 0;
}

void WINAPI stub_LeaveCriticalSection(void* lpCriticalSection) {
    if (!lpCriticalSection) return;
    std::shared_ptr<Win32CSEntry> entry;
    {
        std::lock_guard<std::mutex> lock(gCSTableMutex);
        auto it = gCSTable.find(lpCriticalSection);
        if (it != gCSTable.end()) {
            entry = it->second;
        }
    }
    if (entry) {
        entry->rmtx.unlock();
    }
}

void WINAPI stub_DeleteCriticalSection(void* lpCriticalSection) {
    if (!lpCriticalSection) return;
    std::lock_guard<std::mutex> lock(gCSTableMutex);
    gCSTable.erase(lpCriticalSection);
}

// Win32 SRWLock & ConditionVariable Subsystem
struct Win32SRWEntry {
    std::shared_mutex smtx;
};

struct Win32CVEntry {
    std::condition_variable_any cv;
};

static std::mutex gSRWTableMutex;
static std::unordered_map<void*, std::shared_ptr<Win32SRWEntry>> gSRWTable;

static std::mutex gCVTableMutex;
static std::unordered_map<void*, std::shared_ptr<Win32CVEntry>> gCVTable;

static std::shared_ptr<Win32SRWEntry> getOrCreateSRW(void* pSrw) {
    if (!pSrw) return nullptr;
    std::lock_guard<std::mutex> lock(gSRWTableMutex);
    auto it = gSRWTable.find(pSrw);
    if (it != gSRWTable.end()) {
        return it->second;
    }
    auto entry = std::make_shared<Win32SRWEntry>();
    gSRWTable[pSrw] = entry;
    return entry;
}

static std::shared_ptr<Win32CVEntry> getOrCreateCV(void* pCv) {
    if (!pCv) return nullptr;
    std::lock_guard<std::mutex> lock(gCVTableMutex);
    auto it = gCVTable.find(pCv);
    if (it != gCVTable.end()) {
        return it->second;
    }
    auto entry = std::make_shared<Win32CVEntry>();
    gCVTable[pCv] = entry;
    return entry;
}

void WINAPI stub_InitializeSRWLock(void* SRWLock) {
    if (SRWLock) {
        getOrCreateSRW(SRWLock);
    }
}

void WINAPI stub_AcquireSRWLockExclusive(void* SRWLock) {
    if (!SRWLock) return;
    auto entry = getOrCreateSRW(SRWLock);
    entry->smtx.lock();
}

void WINAPI stub_ReleaseSRWLockExclusive(void* SRWLock) {
    if (!SRWLock) return;
    auto entry = getOrCreateSRW(SRWLock);
    entry->smtx.unlock();
}

void WINAPI stub_AcquireSRWLockShared(void* SRWLock) {
    if (!SRWLock) return;
    auto entry = getOrCreateSRW(SRWLock);
    entry->smtx.lock_shared();
}

void WINAPI stub_ReleaseSRWLockShared(void* SRWLock) {
    if (!SRWLock) return;
    auto entry = getOrCreateSRW(SRWLock);
    entry->smtx.unlock_shared();
}

int32_t WINAPI stub_TryAcquireSRWLockExclusive(void* SRWLock) {
    if (!SRWLock) return 0;
    auto entry = getOrCreateSRW(SRWLock);
    return entry->smtx.try_lock() ? 1 : 0;
}

void WINAPI stub_InitializeConditionVariable(void* ConditionVariable) {
    if (ConditionVariable) {
        getOrCreateCV(ConditionVariable);
    }
}

void WINAPI stub_WakeConditionVariable(void* ConditionVariable) {
    if (!ConditionVariable) return;
    auto entry = getOrCreateCV(ConditionVariable);
    entry->cv.notify_one();
}

void WINAPI stub_WakeAllConditionVariable(void* ConditionVariable) {
    if (!ConditionVariable) return;
    auto entry = getOrCreateCV(ConditionVariable);
    entry->cv.notify_all();
}

int32_t WINAPI stub_SleepConditionVariableSRW(void* ConditionVariable, void* SRWLock, uint32_t dwMilliseconds, uint32_t Flags) {
    if (!ConditionVariable || !SRWLock) return 0;
    auto srwEntry = getOrCreateSRW(SRWLock);
    auto cvEntry = getOrCreateCV(ConditionVariable);

    bool sharedMode = (Flags & 0x01) != 0; // CONDITION_VARIABLE_LOCKMODE_SHARED
    bool awakened = true;

    if (sharedMode) {
        std::shared_lock<std::shared_mutex> lock(srwEntry->smtx, std::adopt_lock);
        if (dwMilliseconds == 0xFFFFFFFF) {
            cvEntry->cv.wait(lock);
        } else {
            auto status = cvEntry->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds));
            awakened = (status == std::cv_status::no_timeout);
        }
        lock.release();
    } else {
        std::unique_lock<std::shared_mutex> lock(srwEntry->smtx, std::adopt_lock);
        if (dwMilliseconds == 0xFFFFFFFF) {
            cvEntry->cv.wait(lock);
        } else {
            auto status = cvEntry->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds));
            awakened = (status == std::cv_status::no_timeout);
        }
        lock.release();
    }

    return awakened ? 1 : 0;
}

void WINAPI stub_InitializeSListHead(void*) {}

int32_t WINAPI stub_IsProcessorFeaturePresent(uint32_t processorFeature) {
    // PF_FASTFAIL_AVAILABLE = 23 (0x17) -> Trả về 0 để CRT không gọi __fastfail!
    if (processorFeature == 23) {
        return 0;
    }
    return 1;
}

int32_t WINAPI stub_IsDebuggerPresent() { return 0; }

// --- Hệ thống Thread-Local Storage (TLS) & Fiber-Local Storage (FLS) ---
namespace {
    constexpr uint32_t kMaxTlsSlots = 128;
    constexpr uint32_t kMaxFlsSlots = 128;
    constexpr uint32_t TLS_OUT_OF_INDEXES = 0xFFFFFFFFU;
    constexpr uint32_t FLS_OUT_OF_INDEXES = 0xFFFFFFFFU;

    static std::mutex gTlsMutex;
    static bool gTlsAllocated[kMaxTlsSlots] = {false};
    static thread_local void* t_tlsSlots[kMaxTlsSlots] = {nullptr};

    static std::mutex gFlsMutex;
    static bool gFlsAllocated[kMaxFlsSlots] = {false};
    static void* gFlsCallbacks[kMaxFlsSlots] = {nullptr};
    static thread_local void* t_flsSlots[kMaxFlsSlots] = {nullptr};
}

uint32_t WINAPI stub_TlsAlloc() {
    std::lock_guard<std::mutex> lock(gTlsMutex);
    for (uint32_t i = 0; i < kMaxTlsSlots; ++i) {
        if (!gTlsAllocated[i]) {
            gTlsAllocated[i] = true;
            t_tlsSlots[i] = nullptr;
            LOGI("stub_TlsAlloc: Đã cấp phát TLS slot %u", i);
            return i;
        }
    }
    LOGE("stub_TlsAlloc: Hết TLS slots!");
    return TLS_OUT_OF_INDEXES;
}

void* WINAPI stub_TlsGetValue(uint32_t dwTlsIndex) {
    if (dwTlsIndex >= kMaxTlsSlots) {
        return nullptr;
    }
    return t_tlsSlots[dwTlsIndex];
}

int32_t WINAPI stub_TlsSetValue(uint32_t dwTlsIndex, void* lpTlsValue) {
    if (dwTlsIndex >= kMaxTlsSlots) {
        return 0; // FALSE
    }
    t_tlsSlots[dwTlsIndex] = lpTlsValue;
    return 1; // TRUE
}

int32_t WINAPI stub_TlsFree(uint32_t dwTlsIndex) {
    std::lock_guard<std::mutex> lock(gTlsMutex);
    if (dwTlsIndex >= kMaxTlsSlots || !gTlsAllocated[dwTlsIndex]) {
        return 0; // FALSE
    }
    gTlsAllocated[dwTlsIndex] = false;
    t_tlsSlots[dwTlsIndex] = nullptr;
    return 1; // TRUE
}

uint32_t WINAPI stub_FlsAlloc(void* lpCallback) {
    std::lock_guard<std::mutex> lock(gFlsMutex);
    for (uint32_t i = 0; i < kMaxFlsSlots; ++i) {
        if (!gFlsAllocated[i]) {
            gFlsAllocated[i] = true;
            gFlsCallbacks[i] = lpCallback;
            t_flsSlots[i] = nullptr;
            LOGI("stub_FlsAlloc: Đã cấp phát FLS slot %u (callback=%p)", i, lpCallback);
            return i;
        }
    }
    LOGE("stub_FlsAlloc: Hết FLS slots!");
    return FLS_OUT_OF_INDEXES;
}

void* WINAPI stub_FlsGetValue(uint32_t dwFlsIndex) {
    if (dwFlsIndex >= kMaxFlsSlots) {
        return nullptr;
    }
    return t_flsSlots[dwFlsIndex];
}

int32_t WINAPI stub_FlsSetValue(uint32_t dwFlsIndex, void* lpFlsData) {
    if (dwFlsIndex >= kMaxFlsSlots) {
        return 0; // FALSE
    }
    t_flsSlots[dwFlsIndex] = lpFlsData;
    return 1; // TRUE
}

int32_t WINAPI stub_FlsFree(uint32_t dwFlsIndex) {
    std::lock_guard<std::mutex> lock(gFlsMutex);
    if (dwFlsIndex >= kMaxFlsSlots || !gFlsAllocated[dwFlsIndex]) {
        return 0; // FALSE
    }
    gFlsAllocated[dwFlsIndex] = false;
    gFlsCallbacks[dwFlsIndex] = nullptr;
    t_flsSlots[dwFlsIndex] = nullptr;
    return 1; // TRUE
}

void* WINAPI stub_EncodePointer(void* ptr) { return ptr; }
void* WINAPI stub_DecodePointer(void* ptr) { return ptr; }

void WINAPI stub_OutputDebugStringW(const char16_t* lpOutputString) {
    if (lpOutputString) {
        std::string s = normalizeWinPathW(lpOutputString);
        LOGI("[GameDebug] %s", s.c_str());
    }
}
void WINAPI stub_GetStartupInfoW(void* lpStartupInfo) {
    if (lpStartupInfo) memset(lpStartupInfo, 0, 68);
}

// Exception Filter stubs
void* WINAPI stub_SetUnhandledExceptionFilter(void*) { return nullptr; }
int32_t WINAPI stub_UnhandledExceptionFilter(void*) { return 0; }
void WINAPI stub_RtlCaptureContext(void*) {}
void* WINAPI stub_RtlLookupFunctionEntry(uint64_t, void*, void*) { return nullptr; }
void* WINAPI stub_RtlVirtualUnwind(uint32_t, uint64_t, uint64_t, void*, void*, void*, void*, void*) { return nullptr; }
void* WINAPI stub_RtlPcToFileHeader(void*, void** baseOfImage) {
    if (baseOfImage) *baseOfImage = gLoadedImageBase;
    return gLoadedImageBase;
}
void WINAPI stub_RaiseException(uint32_t, uint32_t, uint32_t, const uint64_t*) {}
void WINAPI stub_RtlUnwind(void*, void*, void*, void*) {}
void WINAPI stub_RtlUnwindEx(void*, void*, void*, void*, void*, void*) {}
void WINAPI stub_TerminateProcess(void*, uint32_t uExitCode) {
    LOGW("stub_TerminateProcess gọi với mã thoát: %u", uExitCode);
    exit(static_cast<int>(uExitCode));
}
void WINAPI stub_ExitProcess(uint32_t uExitCode) {
    void* returnAddress = __builtin_return_address(0);
    LOGW("stub_ExitProcess gọi với mã thoát: %u, caller: %p", uExitCode, returnAddress);
    if (uExitCode == 3) {
        LOGW("Cảnh báo: stub_ExitProcess(3) từ CRT abort/terminate được ngăn chặn để bảo vệ Game Thread tiếp tục chạy!");
        return;
    }
    exit(static_cast<int>(uExitCode));
}

// Các hàm Stub USER32
void* WINAPI stub_CreateWindowExA(uint32_t, const char*, const char*, uint32_t, int, int, int, int, void*, void*, void*, void*) { return DUMMY_WINDOW; }
void* WINAPI stub_CreateWindowExW(uint32_t, const char16_t*, const char16_t*, uint32_t, int, int, int, int, void*, void*, void*, void*) { return DUMMY_WINDOW; }
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
struct Win32Msg {
    void* hwnd;
    uint32_t message;
    uint64_t wParam;
    int64_t lParam;
    uint32_t time;
    struct {
        int32_t x;
        int32_t y;
    } pt;
    uint32_t lPrivate;
};

static std::queue<Win32Msg> gMsgQueue;
static std::mutex gMsgMutex;
static bool gInitialMessagesPushed = false;

int32_t WINAPI stub_PeekMessageA(void* lpMsg, void*, uint32_t, uint32_t, uint32_t wRemoveMsg) {
    if (!gInitialMessagesPushed) {
        gInitialMessagesPushed = true;
        // Bơm các thông điệp Win32 ban đầu để kích hoạt game engine
        std::lock_guard<std::mutex> lock(gMsgMutex);
        Win32Msg m1{ DUMMY_WINDOW, 0x0001, 0, 0, 0, {960, 540}, 0 }; // WM_CREATE
        Win32Msg m2{ DUMMY_WINDOW, 0x0018, 1, 0, 0, {960, 540}, 0 }; // WM_SHOWWINDOW (SW_SHOW)
        Win32Msg m3{ DUMMY_WINDOW, 0x001C, 1, 0, 0, {960, 540}, 0 }; // WM_ACTIVATEAPP (bActive=TRUE)
        Win32Msg m4{ DUMMY_WINDOW, 0x0006, 1, 0, 0, {960, 540}, 0 }; // WM_ACTIVATE (WA_ACTIVE)
        Win32Msg m5{ DUMMY_WINDOW, 0x0007, 0, 0, 0, {960, 540}, 0 }; // WM_SETFOCUS
        Win32Msg m6{ DUMMY_WINDOW, 0x0005, 0, (1080 << 16) | 1920, 0, {960, 540}, 0 }; // WM_SIZE (1920x1080)
        Win32Msg m7{ DUMMY_WINDOW, 0x000F, 0, 0, 0, {960, 540}, 0 }; // WM_PAINT
        gMsgQueue.push(m1);
        gMsgQueue.push(m2);
        gMsgQueue.push(m3);
        gMsgQueue.push(m4);
        gMsgQueue.push(m5);
        gMsgQueue.push(m6);
        gMsgQueue.push(m7);
        LOGI("stub_PeekMessageA: Đã bơm 7 thông điệp vòng đời khởi tạo (WM_CREATE, SHOWWINDOW, ACTIVATEAPP, ACTIVATE, SETFOCUS, SIZE, PAINT)");
    }

    std::lock_guard<std::mutex> lock(gMsgMutex);
    if (gMsgQueue.empty()) return 0;

    if (lpMsg) {
        *reinterpret_cast<Win32Msg*>(lpMsg) = gMsgQueue.front();
    }
    if (wRemoveMsg & 0x0001) { // PM_REMOVE
        gMsgQueue.pop();
    }
    return 1;
}

int32_t WINAPI stub_TranslateMessage(const void*) { return 1; }

// Game Window Procedure pointer capture
using Win32WndProc = int64_t (WINAPI *)(void* hwnd, uint32_t msg, uint64_t wParam, int64_t lParam);
static Win32WndProc gGameWndProc = nullptr;

int64_t WINAPI stub_DispatchMessageA(const void* lpMsg) {
    if (!lpMsg) return 0;
    const auto* msg = reinterpret_cast<const Win32Msg*>(lpMsg);
    if (gGameWndProc) {
        LOGI("stub_DispatchMessageA: msg=0x%04X, hwnd=%p, wParam=0x%llX, lParam=0x%llX",
             msg->message, msg->hwnd, static_cast<unsigned long long>(msg->wParam),
             static_cast<unsigned long long>(msg->lParam));
        return gGameWndProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }
    return 0;
}

int64_t WINAPI stub_DefWindowProcA(void*, uint32_t msg, uint64_t, int64_t) {
    switch (msg) {
        case 0x0084: return 1; // WM_NCHITTEST -> HTCLIENT
        case 0x0020: return 1; // WM_SETCURSOR -> TRUE
        default: return 0;
    }
}

void WINAPI stub_PostQuitMessage(int32_t) {}

// Window state & focus lifecycle
void* WINAPI stub_GetForegroundWindow() { return DUMMY_WINDOW; }
void* WINAPI stub_GetActiveWindow() { return DUMMY_WINDOW; }
void* WINAPI stub_GetFocus() { return DUMMY_WINDOW; }
void* WINAPI stub_SetActiveWindow(void*) { return DUMMY_WINDOW; }
int32_t WINAPI stub_SetForegroundWindow(void*) { return 1; }
void* WINAPI stub_SetFocus(void*) { return DUMMY_WINDOW; }
int32_t WINAPI stub_IsIconic(void*) { return 0; }
int32_t WINAPI stub_IsZoomed(void*) { return 0; }

static std::atomic<int32_t> gCursorX{960};
static std::atomic<int32_t> gCursorY{540};
static std::atomic<uint8_t> gVirtualKeyState[256]{};

int32_t WINAPI stub_GetCursorPos(void* lpPoint) {
    if (lpPoint) {
        int32_t* pt = reinterpret_cast<int32_t*>(lpPoint);
        pt[0] = gCursorX.load();
        pt[1] = gCursorY.load();
    }
    return 1;
}
int32_t WINAPI stub_SetCursorPos(int32_t x, int32_t y) {
    gCursorX.store(x);
    gCursorY.store(y);
    return 1;
}
int32_t WINAPI stub_ShowCursor(int32_t) { return 0; }
void* WINAPI stub_SetCursor(void*) { return nullptr; }
void* WINAPI stub_LoadCursorA(void*, const char*) { return reinterpret_cast<void*>(0x1234); }
void* WINAPI stub_LoadIconA(void*, const char*) { return reinterpret_cast<void*>(0x5678); }

uint16_t WINAPI stub_RegisterClassExA(const void* lpwcx) {
    if (lpwcx) {
        struct Win32WndClassExA {
            uint32_t cbSize;
            uint32_t style;
            Win32WndProc lpfnWndProc;
            int32_t cbClsExtra;
            int32_t cbWndExtra;
            void* hInstance;
            void* hIcon;
            void* hCursor;
            void* hbrBackground;
            const char* lpszMenuName;
            const char* lpszClassName;
            void* hIconSm;
        };
        const auto* wcx = reinterpret_cast<const Win32WndClassExA*>(lpwcx);
        if (wcx->lpfnWndProc) {
            gGameWndProc = wcx->lpfnWndProc;
            LOGI("stub_RegisterClassExA: Ghi nhận Game WndProc=%p, ClassName='%s'",
                 gGameWndProc, wcx->lpszClassName ? wcx->lpszClassName : "");
        }
    }
    return 0x1234;
}

int32_t WINAPI stub_AdjustWindowRectEx(void*, uint32_t, int32_t, uint32_t) { return 1; }
int32_t WINAPI stub_SystemParametersInfoA(uint32_t, uint32_t, void*, uint32_t) { return 1; }
void* WINAPI stub_MonitorFromWindow(void*, uint32_t) { return reinterpret_cast<void*>(0x9999); }
int32_t WINAPI stub_GetMonitorInfoA(void*, void* lpmi) {
    if (lpmi) {
        struct Win32MonitorInfo {
            uint32_t cbSize;
            int32_t rcMonitor[4];
            int32_t rcWork[4];
            uint32_t dwFlags;
        };
        auto* mi = reinterpret_cast<Win32MonitorInfo*>(lpmi);
        mi->rcMonitor[0] = 0; mi->rcMonitor[1] = 0; mi->rcMonitor[2] = 1920; mi->rcMonitor[3] = 1080;
        mi->rcWork[0] = 0; mi->rcWork[1] = 0; mi->rcWork[2] = 1920; mi->rcWork[3] = 1080;
        mi->dwFlags = 1;
    }
    return 1;
}
void* WINAPI stub_WindowFromPoint(int32_t, int32_t) { return DUMMY_WINDOW; }
int32_t WINAPI stub_ScreenToClient(void*, void*) { return 1; }
int32_t WINAPI stub_ClientToScreen(void*, void*) { return 1; }
int32_t WINAPI stub_TrackMouseEvent(void*) { return 1; }
uintptr_t WINAPI stub_SetTimer(void*, uintptr_t nIDEvent, uint32_t, void*) { return nIDEvent ? nIDEvent : 1; }
int32_t WINAPI stub_KillTimer(void*, uintptr_t) { return 1; }
int16_t WINAPI stub_GetKeyState(int32_t vKey) {
    if (vKey >= 0 && vKey < 256) {
        return gVirtualKeyState[vKey].load() ? static_cast<int16_t>(0x8000) : 0;
    }
    return 0;
}
int32_t WINAPI stub_GetKeyboardState(void* lpKeyState) {
    if (lpKeyState) {
        uint8_t* states = reinterpret_cast<uint8_t*>(lpKeyState);
        for (int i = 0; i < 256; ++i) {
            states[i] = gVirtualKeyState[i].load() ? 0x80 : 0x00;
        }
    }
    return 1;
}
int32_t WINAPI stub_AttachThreadInput(uint32_t, uint32_t, int32_t) { return 1; }

static std::mutex gWindowLongMutex;
static std::map<std::pair<void*, int32_t>, intptr_t> gWindowLongMap;

int32_t WINAPI stub_GetWindowLongA(void* hWnd, int32_t nIndex) {
    std::lock_guard<std::mutex> lock(gWindowLongMutex);
    auto it = gWindowLongMap.find({hWnd, nIndex});
    if (it != gWindowLongMap.end()) {
        return static_cast<int32_t>(it->second);
    }
    if (nIndex == -16) return 0x10000000; // GWL_STYLE: WS_VISIBLE
    return 0;
}

int32_t WINAPI stub_SetWindowLongA(void* hWnd, int32_t nIndex, int32_t dwNewLong) {
    std::lock_guard<std::mutex> lock(gWindowLongMutex);
    int32_t old = 0;
    auto it = gWindowLongMap.find({hWnd, nIndex});
    if (it != gWindowLongMap.end()) {
        old = static_cast<int32_t>(it->second);
    }
    gWindowLongMap[{hWnd, nIndex}] = dwNewLong;
    return old;
}

intptr_t WINAPI stub_GetWindowLongPtrA(void* hWnd, int32_t nIndex) {
    std::lock_guard<std::mutex> lock(gWindowLongMutex);
    auto it = gWindowLongMap.find({hWnd, nIndex});
    if (it != gWindowLongMap.end()) {
        return it->second;
    }
    if (nIndex == -16) return 0x10000000; // GWL_STYLE: WS_VISIBLE
    return 0;
}

intptr_t WINAPI stub_SetWindowLongPtrA(void* hWnd, int32_t nIndex, intptr_t dwNewLong) {
    std::lock_guard<std::mutex> lock(gWindowLongMutex);
    intptr_t old = 0;
    auto it = gWindowLongMap.find({hWnd, nIndex});
    if (it != gWindowLongMap.end()) {
        old = it->second;
    }
    gWindowLongMap[{hWnd, nIndex}] = dwNewLong;
    return old;
}

int32_t WINAPI stub_GetWindowPlacement(void*, void* lpwndpl) {
    if (lpwndpl) {
        struct Win32WindowPlacement {
            uint32_t length;
            uint32_t flags;
            uint32_t showCmd;
            int32_t ptMinPosition[2];
            int32_t ptMaxPosition[2];
            int32_t rcNormalPosition[4];
        };
        auto* wp = reinterpret_cast<Win32WindowPlacement*>(lpwndpl);
        wp->showCmd = 1;
        wp->rcNormalPosition[0] = 0; wp->rcNormalPosition[1] = 0; wp->rcNormalPosition[2] = 1920; wp->rcNormalPosition[3] = 1080;
    }
    return 1;
}
int32_t WINAPI stub_InvalidateRect(void*, const void*, int32_t) { return 1; }
int64_t WINAPI stub_SendMessageA(void*, uint32_t, uint64_t, int64_t) { return 0; }
int64_t WINAPI stub_CallWindowProcA(void*, void*, uint32_t, uint64_t, int64_t) { return 0; }
int32_t WINAPI stub_EndDialog(void*, intptr_t) { return 1; }
void* WINAPI stub_GetDlgItem(void*, int32_t) { return DUMMY_WINDOW; }

int32_t WINAPI stub_GetWindowTextW(void*, char16_t* lpString, int32_t nMaxCount) {
    if (lpString && nMaxCount > 0) lpString[0] = u'\0';
    return 0;
}
int32_t WINAPI stub_GetWindowTextLengthW(void*) { return 0; }
int32_t WINAPI stub_SetWindowTextW(void*, const char16_t*) { return 1; }
int32_t WINAPI stub_SetDlgItemTextW(void*, int32_t, const char16_t*) { return 1; }
uint32_t WINAPI stub_GetDlgItemTextW(void*, int32_t, char16_t* lpString, int32_t nMaxCount) {
    if (lpString && nMaxCount > 0) lpString[0] = u'\0';
    return 0;
}
int64_t WINAPI stub_SendMessageW(void*, uint32_t, uint64_t, int64_t) { return 0; }
int64_t WINAPI stub_DialogBoxParamW(void*, const char16_t*, void*, void*, int64_t) { return 1; }

uint32_t WINAPI stub_GetWindowThreadProcessId(void*, uint32_t* lpdwProcessId) {
    if (lpdwProcessId) *lpdwProcessId = static_cast<uint32_t>(getpid());
    return static_cast<uint32_t>(pthread_self());
}
intptr_t WINAPI stub_SetClassLongPtrA(void*, int32_t, intptr_t) { return 0; }
void* WINAPI stub_FindWindowA(const char*, const char*) { return DUMMY_WINDOW; }

struct Win32PropKey {
    void* hwnd;
    uint16_t atom;
    std::string str;

    bool operator<(const Win32PropKey& o) const {
        if (hwnd != o.hwnd) return hwnd < o.hwnd;
        if (atom != o.atom) return atom < o.atom;
        return str < o.str;
    }
};

static std::mutex gPropMutex;
static std::map<Win32PropKey, void*> gPropMap;

int32_t WINAPI stub_SetPropA(void* hWnd, const char* lpString, void* hData) {
    if (!hWnd || !lpString) return 0;
    Win32PropKey key{};
    key.hwnd = hWnd;
    if (reinterpret_cast<uintptr_t>(lpString) <= 0xFFFF) {
        key.atom = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpString));
    } else {
        key.atom = 0;
        key.str = lpString;
    }
    std::lock_guard<std::mutex> lock(gPropMutex);
    gPropMap[key] = hData;
    LOGI("stub_SetPropA: hwnd=%p, prop='%s', atom=%u -> data=%p",
         hWnd, key.str.c_str(), key.atom, hData);
    return 1;
}

void* WINAPI stub_GetPropA(void* hWnd, const char* lpString) {
    if (!hWnd || !lpString) return nullptr;
    Win32PropKey key{};
    key.hwnd = hWnd;
    if (reinterpret_cast<uintptr_t>(lpString) <= 0xFFFF) {
        key.atom = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpString));
    } else {
        key.atom = 0;
        key.str = lpString;
    }
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gPropMap.find(key);
    if (it != gPropMap.end()) {
        return it->second;
    }
    return nullptr;
}

void* WINAPI stub_RemovePropA(void* hWnd, const char* lpString) {
    if (!hWnd || !lpString) return nullptr;
    Win32PropKey key{};
    key.hwnd = hWnd;
    if (reinterpret_cast<uintptr_t>(lpString) <= 0xFFFF) {
        key.atom = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(lpString));
    } else {
        key.atom = 0;
        key.str = lpString;
    }
    std::lock_guard<std::mutex> lock(gPropMutex);
    auto it = gPropMap.find(key);
    if (it != gPropMap.end()) {
        void* val = it->second;
        gPropMap.erase(it);
        LOGI("stub_RemovePropA: hwnd=%p, prop='%s', atom=%u -> removed %p",
             hWnd, key.str.c_str(), key.atom, val);
        return val;
    }
    return nullptr;
}
int32_t WINAPI stub_EnumDisplayMonitors(void*, void*, int32_t (WINAPI *lpfnEnum)(void*, void*, void*, int64_t), int64_t lParam) {
    if (lpfnEnum) {
        lpfnEnum(reinterpret_cast<void*>(0x9999), nullptr, nullptr, lParam);
    }
    return 1;
}
void* WINAPI stub_MonitorFromRect(const void*, uint32_t) { return reinterpret_cast<void*>(0x9999); }

// GDI32 Subsystem
int32_t WINAPI stub_GetDeviceCaps(void*, int32_t nIndex) {
    switch (nIndex) {
        case 8: return 1920;  // HORZRES
        case 10: return 1080; // VERTRES
        case 12: return 32;   // BITSPIXEL
        case 14: return 1;    // PLANES
        case 88: return 96;   // LOGPIXELSX
        case 90: return 96;   // LOGPIXELSY
        default: return 1;
    }
}
void* WINAPI stub_GetStockObject(int32_t) { return reinterpret_cast<void*>(0x8888); }

// WINMM Subsystem
uint32_t WINAPI stub_timeBeginPeriod(uint32_t) { return 0; }
uint32_t WINAPI stub_timeEndPeriod(uint32_t) { return 0; }
int32_t WINAPI stub_PlaySoundA(const char*, void*, uint32_t) { return 1; }

// OLE32 Subsystem
int32_t WINAPI stub_CoInitializeEx(void*, uint32_t) { return 0; } // S_OK
void WINAPI stub_CoUninitialize() {}
int32_t WINAPI stub_CoCreateGuid(void* pguid) {
    if (pguid) {
        static uint32_t seed = 0xABCDEF01;
        seed += 13;
        memset(pguid, static_cast<int>(seed & 0xFF), 16);
    }
    return 0; // S_OK
}
int32_t WINAPI stub_CoCreateInstance(const void* rclsid, void*, uint32_t, const void* riid, void** ppv) {
    if (ppv) *ppv = nullptr;
    if (!ppv) return static_cast<int32_t>(0x80004003); // E_POINTER

    // CLSID_CColorConvertDMO / VideoProcessorMFT: {98230571-0087-4204-B020-3282538E57D3}
    static const uint8_t kClsidColorConvertDmo[16] = {
        0x71, 0x05, 0x23, 0x98, 0x87, 0x00, 0x04, 0x42, 0xB0, 0x20, 0x32, 0x82, 0x53, 0x8E, 0x57, 0xD3
    };
    // IID_IMFTransform: {BF94C121-5B05-4E6F-8000-BA598961414D}
    static const uint8_t kIidImfTransform[16] = {
        0x21, 0xC1, 0x94, 0xBF, 0x05, 0x5B, 0x6F, 0x4E, 0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D
    };

    bool isTransform = false;
    if (rclsid && memcmp(rclsid, kClsidColorConvertDmo, 16) == 0) isTransform = true;
    if (riid && memcmp(riid, kIidImfTransform, 16) == 0) isTransform = true;

    if (isTransform) {
        int32_t hr = MediaFoundationBridge::createColorConvertMft(ppv);
        LOGI("stub_CoCreateInstance: Tao MFColorConvertMftImpl cho Video Processor thanh cong -> ppv=%p", *ppv);
        return hr;
    }

    LOGW("stub_CoCreateInstance: CLSID/IID chua duoc ho tro (rclsid=%p, riid=%p) -> E_NOINTERFACE", rclsid, riid);
    return static_cast<int32_t>(0x80004002); // E_NOINTERFACE
}
void WINAPI stub_CoTaskMemFree(void* ptr) { if (ptr) free(ptr); }
int32_t WINAPI stub_PropVariantClear(void* pvar) {
    if (pvar) memset(pvar, 0, 24);
    return 0;
}

// ADVAPI32 Crypto Subsystem
int32_t WINAPI stub_CryptAcquireContextA(void** phProv, const char*, const char*, uint32_t, uint32_t) {
    if (phProv) *phProv = reinterpret_cast<void*>(0x70001);
    return 1;
}
int32_t WINAPI stub_CryptReleaseContext(void*, uint32_t) { return 1; }
int32_t WINAPI stub_CryptImportKey(void*, const void*, uint32_t, void*, uint32_t, void** phKey) {
    if (phKey) *phKey = reinterpret_cast<void*>(0x70002);
    return 1;
}
int32_t WINAPI stub_CryptSetKeyParam(void*, uint32_t, const void*, uint32_t) { return 1; }
int32_t WINAPI stub_CryptEncrypt(void*, void*, int32_t, uint32_t, uint8_t*, uint32_t*, uint32_t) { return 1; }
int32_t WINAPI stub_CryptDecrypt(void*, void*, int32_t, uint32_t, uint8_t*, uint32_t*) { return 1; }
int32_t WINAPI stub_CryptDestroyKey(void*) { return 1; }
int32_t WINAPI stub_CryptDestroyHash(void*) { return 1; }
int32_t WINAPI stub_CryptGenRandom(void*, uint32_t dwLen, uint8_t* pbBuffer) {
    if (pbBuffer != nullptr && dwLen > 0) {
        arc4random_buf(pbBuffer, dwLen);
        return 1;
    }
    return 0;
}
uint8_t WINAPI stub_SystemFunction036(void* randomBuffer, uint32_t randomBufferLength) {
    if (randomBuffer != nullptr && randomBufferLength > 0) {
        arc4random_buf(randomBuffer, randomBufferLength);
        return 1;
    }
    return 0;
}

// KERNEL32 Misc & Locale
int32_t WINAPI stub_GetCPInfo(uint32_t, void* lpCPInfo) {
    if (lpCPInfo) {
        struct CPINFO {
            uint32_t MaxCharSize;
            uint8_t DefaultChar[2];
            uint8_t LeadByte[12];
        };
        auto* cp = reinterpret_cast<CPINFO*>(lpCPInfo);
        cp->MaxCharSize = 2;
    }
    return 1;
}
int32_t WINAPI stub_GetLocaleInfoW(uint32_t, uint32_t, char16_t* lpLCData, int32_t cchData) {
    if (lpLCData && cchData > 0) lpLCData[0] = 0;
    return 1;
}
uint32_t WINAPI stub_GetUserDefaultLCID() { return 0x0409; }
int32_t WINAPI stub_IsValidLocale(uint32_t, uint32_t) { return 1; }
int32_t WINAPI stub_EnumSystemLocalesW(void*, uint32_t) { return 1; }
int32_t WINAPI stub_CompareStringW(uint32_t, uint32_t, const char16_t* lpStr1, int32_t cch1, const char16_t* lpStr2, int32_t cch2) {
    if (!lpStr1 || !lpStr2) return 2; // CSTR_EQUAL
    size_t len1 = cch1 >= 0 ? cch1 : utf16Len(lpStr1);
    size_t len2 = cch2 >= 0 ? cch2 : utf16Len(lpStr2);
    size_t minLen = std::min(len1, len2);
    for (size_t i = 0; i < minLen; ++i) {
        if (lpStr1[i] < lpStr2[i]) return 1; // CSTR_LESS_THAN
        if (lpStr1[i] > lpStr2[i]) return 3; // CSTR_GREATER_THAN
    }
    if (len1 < len2) return 1;
    if (len1 > len2) return 3;
    return 2; // CSTR_EQUAL
}
int32_t WINAPI stub_LCMapStringW(uint32_t, uint32_t, const char16_t*, int32_t cchSrc, char16_t* lpDestStr, int32_t cchDest) {
    if (cchDest > 0 && lpDestStr) lpDestStr[0] = 0;
    return cchSrc > 0 ? cchSrc : 1;
}
int32_t WINAPI stub_LCMapStringEx(const char16_t*, uint32_t, const char16_t*, int32_t cchSrc, char16_t* lpDestStr, int32_t cchDest, void*, void*, intptr_t) {
    if (cchDest > 0 && lpDestStr) lpDestStr[0] = 0;
    return cchSrc > 0 ? cchSrc : 1;
}
int32_t WINAPI stub_GetStringTypeW(uint32_t, const char16_t*, int32_t cchSrc, uint16_t* lpCharType) {
    if (lpCharType && cchSrc > 0) {
        for (int32_t i = 0; i < cchSrc; ++i) lpCharType[i] = 0x0008; // C1_ALPHA
    }
    return 1;
}
uint32_t WINAPI stub_GetTimeZoneInformation(void* lpTimeZoneInformation) {
    time_t now = time(nullptr);
    struct tm localTm{};
    bool hasLocal = (localtime_r(&now, &localTm) != nullptr);
    if (lpTimeZoneInformation) {
        memset(lpTimeZoneInformation, 0, 172);
        if (hasLocal) {
            auto* biasPtr = reinterpret_cast<int32_t*>(lpTimeZoneInformation);
            biasPtr[0] = -static_cast<int32_t>(localTm.tm_gmtoff / 60);
            if (localTm.tm_isdst > 0) {
                // DaylightBias nằm tại offset 0xA8 (168 bytes = 42 * 4)
                biasPtr[42] = -60;
            }
        }
    }
    if (hasLocal && localTm.tm_isdst > 0) {
        return 2; // TIME_ZONE_ID_DAYLIGHT
    }
    return hasLocal ? 1 : 0; // TIME_ZONE_ID_STANDARD / UNKNOWN
}
int32_t WINAPI stub_GetVersionExA(void* lpVersionInformation) {
    if (lpVersionInformation) {
        struct OSVERSIONINFOA {
            uint32_t dwOSVersionInfoSize;
            uint32_t dwMajorVersion;
            uint32_t dwMinorVersion;
            uint32_t dwBuildNumber;
            uint32_t dwPlatformId;
            char szCSDVersion[128];
        };
        auto* vi = reinterpret_cast<OSVERSIONINFOA*>(lpVersionInformation);
        vi->dwMajorVersion = 10;
        vi->dwMinorVersion = 0;
        vi->dwBuildNumber = 19045;
        vi->dwPlatformId = 2; // VER_PLATFORM_WIN32_NT
        vi->szCSDVersion[0] = '\0';
    }
    return 1;
}
int32_t WINAPI stub_SetEnvironmentVariableW(const char16_t*, const char16_t*) { return 1; }

namespace {

static constexpr uint32_t WIN32_FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x00000100u;
static constexpr uint32_t WIN32_FORMAT_MESSAGE_FROM_STRING     = 0x00000400u;
static constexpr uint32_t WIN32_ERROR_SUCCESS                  = 0u;
static constexpr uint32_t WIN32_ERROR_FILE_NOT_FOUND           = 2u;
static constexpr uint32_t WIN32_ERROR_PATH_NOT_FOUND           = 3u;
static constexpr uint32_t WIN32_ERROR_ACCESS_DENIED            = 5u;
static constexpr uint32_t WIN32_ERROR_INVALID_HANDLE           = 6u;
static constexpr uint32_t WIN32_ERROR_NO_MORE_FILES            = 18u;
static constexpr uint32_t WIN32_ERROR_INVALID_PARAMETER        = 87u;
static constexpr uint32_t WIN32_ERROR_ALREADY_EXISTS           = 183u;

// Ánh xạ mã lỗi Win32 sang thông báo mô tả dễ hiểu cho engine khi ghi log lỗi
std::string resolveWin32ErrorMessage(uint32_t dwFlags, const void* lpSource, uint32_t dwMessageId) {
    if ((dwFlags & WIN32_FORMAT_MESSAGE_FROM_STRING) != 0 && lpSource != nullptr) {
        return std::string(reinterpret_cast<const char*>(lpSource));
    }
    switch (dwMessageId) {
        case WIN32_ERROR_SUCCESS: return "The operation completed successfully.";
        case WIN32_ERROR_FILE_NOT_FOUND: return "The system cannot find the file specified.";
        case WIN32_ERROR_PATH_NOT_FOUND: return "The system cannot find the path specified.";
        case WIN32_ERROR_ACCESS_DENIED: return "Access is denied.";
        case WIN32_ERROR_INVALID_HANDLE: return "The handle is invalid.";
        case WIN32_ERROR_NO_MORE_FILES: return "There are no more files.";
        case WIN32_ERROR_INVALID_PARAMETER: return "The parameter is incorrect.";
        case WIN32_ERROR_ALREADY_EXISTS: return "Cannot create a file when that file already exists.";
        default: return "Win32 error " + std::to_string(dwMessageId);
    }
}

} // namespace

uint32_t WINAPI stub_FormatMessageA(uint32_t dwFlags, const void* lpSource, uint32_t dwMessageId,
                                    uint32_t, char* lpBuffer, uint32_t nSize, void*) {
    if (!lpBuffer) {
        stub_SetLastError(WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    const std::string message = resolveWin32ErrorMessage(dwFlags, lpSource, dwMessageId);
    const size_t msgLen = message.size();

    // Khi có cờ ALLOCATE_BUFFER, lpBuffer thực chất là con trỏ kép char** nhận vùng nhớ LocalFree-able
    if ((dwFlags & WIN32_FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0) {
        char** outPtr = reinterpret_cast<char**>(lpBuffer);
        const size_t allocBytes = std::max<size_t>(msgLen + 1, static_cast<size_t>(nSize));
        char* allocated = static_cast<char*>(malloc(allocBytes));
        if (!allocated) {
            *outPtr = nullptr;
            return 0;
        }
        memcpy(allocated, message.data(), msgLen);
        allocated[msgLen] = '\0';
        *outPtr = allocated;
        return static_cast<uint32_t>(msgLen);
    }

    if (nSize == 0) {
        return 0;
    }
    const size_t copyLen = std::min<size_t>(msgLen, static_cast<size_t>(nSize) - 1);
    if (copyLen > 0) {
        memcpy(lpBuffer, message.data(), copyLen);
    }
    lpBuffer[copyLen] = '\0';
    return static_cast<uint32_t>(copyLen);
}
void* WINAPI stub_FindResourceA(void*, const char*, const char*) { return reinterpret_cast<void*>(0x3001); }
void* WINAPI stub_LoadResource(void*, void*) { return reinterpret_cast<void*>(0x3002); }
void* WINAPI stub_LockResource(void*) { return reinterpret_cast<void*>(0x3003); }
uint32_t WINAPI stub_SizeofResource(void*, void*) { return 0; }
int32_t WINAPI stub_FreeResource(void*) { return 1; }

// Network & Hardware Identification Stubs
int32_t WINAPI stub_GetAdaptersInfo(void*, uint32_t* pOutBufLen) {
    if (pOutBufLen) *pOutBufLen = 0;
    return 232; // ERROR_NO_DATA
}
int8_t WINAPI stub_Netbios(void*) { return 0x23; }
int32_t WINAPI stub_PropVariantToInt64(const void*, int64_t* pret) {
    if (pret) *pret = 0;
    return 0; // S_OK
}

// WinMM (Wave Audio) API Implementation qua AudioSink
uint32_t WINAPI stub_waveOutGetNumDevs() {
    return AudioSink_WaveOutGetNumDevs();
}
uint32_t WINAPI stub_waveOutGetDevCapsA(uintptr_t uDevId, void* pwoc, uint32_t cbwoc) {
    return AudioSink_WaveOutGetDevCapsA(uDevId, pwoc, cbwoc);
}
uint32_t WINAPI stub_waveOutOpen(void** phwo, uint32_t uDevId, const void* pwfx, uintptr_t dwCallback, uintptr_t dwInstance, uint32_t fdwOpen) {
    return AudioSink_WaveOutOpen(phwo, uDevId, pwfx, dwCallback, dwInstance, fdwOpen);
}
uint32_t WINAPI stub_waveOutClose(void* hwo) {
    return AudioSink_WaveOutClose(hwo);
}
uint32_t WINAPI stub_waveOutPrepareHeader(void* hwo, void* pwh, uint32_t cbwh) {
    return AudioSink_WaveOutPrepareHeader(hwo, pwh, cbwh);
}
uint32_t WINAPI stub_waveOutUnprepareHeader(void* hwo, void* pwh, uint32_t cbwh) {
    return AudioSink_WaveOutUnprepareHeader(hwo, pwh, cbwh);
}
uint32_t WINAPI stub_waveOutWrite(void* hwo, void* pwh, uint32_t cbwh) {
    return AudioSink_WaveOutWrite(hwo, pwh, cbwh);
}
uint32_t WINAPI stub_waveOutReset(void* hwo) {
    return AudioSink_WaveOutReset(hwo);
}
uint32_t WINAPI stub_waveOutPause(void* hwo) {
    return AudioSink_WaveOutPause(hwo);
}
uint32_t WINAPI stub_waveOutRestart(void* hwo) {
    return AudioSink_WaveOutRestart(hwo);
}
uint32_t WINAPI stub_waveOutGetPosition(void* hwo, void* pmmt, uint32_t cbmmt) {
    return AudioSink_WaveOutGetPosition(hwo, pmmt, cbmmt);
}

uint32_t WINAPI stub_waveInGetNumDevs() { return 1; }
uint32_t WINAPI stub_waveInGetDevCapsA(uintptr_t, void* pwic, uint32_t cbwic) {
    if (pwic && cbwic > 0) memset(pwic, 0, cbwic);
    return 0; // MMSYSERR_NOERROR
}
uint32_t WINAPI stub_waveInOpen(void** phwi, uint32_t, const void*, uintptr_t, uintptr_t, uint32_t) {
    if (phwi) *phwi = reinterpret_cast<void*>(0x8002);
    return 0; // MMSYSERR_NOERROR
}
uint32_t WINAPI stub_waveInClose(void*) { return 0; }
uint32_t WINAPI stub_waveInPrepareHeader(void*, void*, uint32_t) { return 0; }
uint32_t WINAPI stub_waveInUnprepareHeader(void*, void*, uint32_t) { return 0; }
uint32_t WINAPI stub_waveInAddBuffer(void*, void*, uint32_t) { return 0; }
uint32_t WINAPI stub_waveInStart(void*) { return 0; }
uint32_t WINAPI stub_waveInStop(void*) { return 0; }
uint32_t WINAPI stub_waveInReset(void*) { return 0; }

// Bảng ánh xạ hàm API
static std::unordered_map<std::string, void*> gFunctionMap;

} // namespace

extern "C" void Win32Bridge_SetLoadedImageBase(void* imageBase) {
    gLoadedImageBase = imageBase;
    LOGI("Win32Bridge đã thiết lập Loaded ImageBase: %p", imageBase);
}

static bool copyFilePreservingData(const std::string& srcPath, const std::string& dstPath) {
    int inFd = open(srcPath.c_str(), O_RDONLY);
    if (inFd < 0) return false;
    int outFd = open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (outFd < 0) {
        close(inFd);
        return false;
    }

    char buf[8192];
    bool ok = true;
    while (true) {
        ssize_t n = read(inFd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(outFd, buf + written, static_cast<size_t>(n - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }
    if (ok) fsync(outFd);
    close(outFd);
    close(inFd);
    return ok;
}

static void migrateLegacyDirRecursive(const std::string& srcDir, const std::string& dstDir, size_t& migratedCount) {
    mkdir(dstDir.c_str(), 0777);
    DIR* dir = opendir(srcDir.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        std::string srcItem = srcDir + "/" + entry->d_name;
        std::string dstItem = resolveCaseInsensitiveSubPath(dstDir, entry->d_name);

        struct stat stSrc{};
        if (stat(srcItem.c_str(), &stSrc) != 0) continue;

        if (S_ISDIR(stSrc.st_mode)) {
            migrateLegacyDirRecursive(srcItem, dstItem, migratedCount);
            rmdir(srcItem.c_str());
        } else if (S_ISREG(stSrc.st_mode)) {
            struct stat stDst{};
            bool dstExists = (stat(dstItem.c_str(), &stDst) == 0);
            if (!dstExists || stSrc.st_mtime > stDst.st_mtime) {
                bool moved = (rename(srcItem.c_str(), dstItem.c_str()) == 0);
                if (!moved && copyFilePreservingData(srcItem, dstItem)) {
                    unlink(srcItem.c_str());
                    moved = true;
                }
                if (moved) ++migratedCount;
            } else {
                // Bản trong saves/ mới hơn hoặc cùng thời điểm, xóa bản trùng cũ
                unlink(srcItem.c_str());
            }
        }
    }
    closedir(dir);
}

static void migrateLegacyDocumentsToSaves(const std::string& dataDir) {
    DIR* dir = opendir(dataDir.c_str());
    if (!dir) return;

    std::string legacyDocsPath;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcasecmp(entry->d_name, "<Documents>") == 0) {
            legacyDocsPath = dataDir + "/" + entry->d_name;
            break;
        }
    }
    closedir(dir);

    if (legacyDocsPath.empty()) return;

    std::string savesRoot = dataDir + "/saves";
    size_t migratedCount = 0;
    migrateLegacyDirRecursive(legacyDocsPath, savesRoot, migratedCount);
    rmdir(legacyDocsPath.c_str());
    LOGI("VFS Migrate Legacy Saves: Da di chuyen %zu file tu '%s' sang '%s'",
         migratedCount, legacyDocsPath.c_str(), savesRoot.c_str());
}

extern "C" void Win32Bridge_SetDataDirectory(const char* dataDir) {
    if (dataDir && dataDir[0] != '\0') {
        gVfsDataDir = dataDir;
        LOGI("Win32Bridge VFS: Gán thư mục dữ liệu gốc = '%s'", gVfsDataDir.c_str());
        migrateLegacyDocumentsToSaves(gVfsDataDir);
    }
}

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
    gFunctionMap["TryEnterCriticalSection"] = reinterpret_cast<void*>(&stub_TryEnterCriticalSection);
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
    gFunctionMap["PeekMessageW"] = reinterpret_cast<void*>(&stub_PeekMessageA);
    gFunctionMap["GetMessageA"] = reinterpret_cast<void*>(&stub_PeekMessageA);
    gFunctionMap["GetMessageW"] = reinterpret_cast<void*>(&stub_PeekMessageA);
    gFunctionMap["TranslateMessage"] = reinterpret_cast<void*>(&stub_TranslateMessage);
    gFunctionMap["DispatchMessageA"] = reinterpret_cast<void*>(&stub_DispatchMessageA);
    gFunctionMap["DispatchMessageW"] = reinterpret_cast<void*>(&stub_DispatchMessageA);
    gFunctionMap["DefWindowProcA"] = reinterpret_cast<void*>(&stub_DefWindowProcA);
    gFunctionMap["DefWindowProcW"] = reinterpret_cast<void*>(&stub_DefWindowProcA);
    gFunctionMap["GetAsyncKeyState"] = reinterpret_cast<void*>(&stub_GetKeyState);
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

    // Hardened Heap & Virtual Memory API Registration
    gFunctionMap["HeapCreate"] = reinterpret_cast<void*>(&stub_HeapCreate);
    gFunctionMap["HeapDestroy"] = reinterpret_cast<void*>(&stub_HeapDestroy);
    gFunctionMap["HeapValidate"] = reinterpret_cast<void*>(&stub_HeapValidate);
    gFunctionMap["VirtualAlloc"] = reinterpret_cast<void*>(&stub_VirtualAlloc);
    gFunctionMap["VirtualFree"] = reinterpret_cast<void*>(&stub_VirtualFree);
    gFunctionMap["VirtualProtect"] = reinterpret_cast<void*>(&stub_VirtualProtect);
    gFunctionMap["VirtualQuery"] = reinterpret_cast<void*>(&stub_VirtualQuery);
    gFunctionMap["GlobalAlloc"] = reinterpret_cast<void*>(&stub_GlobalAlloc);
    gFunctionMap["GlobalFree"] = reinterpret_cast<void*>(&stub_GlobalFree);
    gFunctionMap["LocalFree"] = reinterpret_cast<void*>(&stub_LocalFree);

    // System Information Registration
    gFunctionMap["GetSystemInfo"] = reinterpret_cast<void*>(&stub_GetSystemInfo);
    gFunctionMap["GetNativeSystemInfo"] = reinterpret_cast<void*>(&stub_GetNativeSystemInfo);

    // String & Conversion Registration
    gFunctionMap["MultiByteToWideChar"] = reinterpret_cast<void*>(&stub_MultiByteToWideChar);
    gFunctionMap["WideCharToMultiByte"] = reinterpret_cast<void*>(&stub_WideCharToMultiByte);
    gFunctionMap["lstrlenA"] = reinterpret_cast<void*>(&stub_lstrlenA);
    gFunctionMap["lstrcpynA"] = reinterpret_cast<void*>(&stub_lstrcpynA);

    // Synchronization Registration
    gFunctionMap["CreateEventA"] = reinterpret_cast<void*>(&stub_CreateEventA);
    gFunctionMap["CreateEventW"] = reinterpret_cast<void*>(&stub_CreateEventW);
    gFunctionMap["CreateEventExA"] = reinterpret_cast<void*>(&stub_CreateEventExA);
    gFunctionMap["CreateEventExW"] = reinterpret_cast<void*>(&stub_CreateEventExW);
    gFunctionMap["SetEvent"] = reinterpret_cast<void*>(&stub_SetEvent);
    gFunctionMap["ResetEvent"] = reinterpret_cast<void*>(&stub_ResetEvent);
    gFunctionMap["CreateSemaphoreA"] = reinterpret_cast<void*>(&stub_CreateSemaphoreA);
    gFunctionMap["CreateSemaphoreW"] = reinterpret_cast<void*>(&stub_CreateSemaphoreW);
    gFunctionMap["ReleaseSemaphore"] = reinterpret_cast<void*>(&stub_ReleaseSemaphore);
    gFunctionMap["WaitForSingleObject"] = reinterpret_cast<void*>(&stub_WaitForSingleObject);
    gFunctionMap["WaitForSingleObjectEx"] = reinterpret_cast<void*>(&stub_WaitForSingleObjectEx);
    gFunctionMap["SwitchToThread"] = reinterpret_cast<void*>(&stub_SwitchToThread);

    // Console & Stdio Registration
    gFunctionMap["GetStdHandle"] = reinterpret_cast<void*>(&stub_GetStdHandle);
    gFunctionMap["SetStdHandle"] = reinterpret_cast<void*>(&stub_SetStdHandle);
    gFunctionMap["GetFileType"] = reinterpret_cast<void*>(&stub_GetFileType);
    gFunctionMap["GetConsoleMode"] = reinterpret_cast<void*>(&stub_GetConsoleMode);
    gFunctionMap["WriteConsoleW"] = reinterpret_cast<void*>(&stub_WriteConsoleW);
    gFunctionMap["ReadConsoleW"] = reinterpret_cast<void*>(&stub_ReadConsoleW);

    // File & Path Information Registration
    gFunctionMap["GetFileAttributesW"] = reinterpret_cast<void*>(&stub_GetFileAttributesW);
    gFunctionMap["GetFileAttributesA"] = reinterpret_cast<void*>(&stub_GetFileAttributesA);
    gFunctionMap["GetCurrentDirectoryW"] = reinterpret_cast<void*>(&stub_GetCurrentDirectoryW);
    gFunctionMap["GetCurrentDirectoryA"] = reinterpret_cast<void*>(&stub_GetCurrentDirectoryA);
    gFunctionMap["SetCurrentDirectoryW"] = reinterpret_cast<void*>(&stub_SetCurrentDirectoryW);
    gFunctionMap["SetCurrentDirectoryA"] = reinterpret_cast<void*>(&stub_SetCurrentDirectoryA);
    gFunctionMap["GetModuleFileNameA"] = reinterpret_cast<void*>(&stub_GetModuleFileNameA);
    gFunctionMap["GetModuleFileNameW"] = reinterpret_cast<void*>(&stub_GetModuleFileNameW);

    // Virtual File System (VFS) File Operations Registration
    gFunctionMap["CreateFileW"] = reinterpret_cast<void*>(&stub_CreateFileW);
    gFunctionMap["CreateFileA"] = reinterpret_cast<void*>(&stub_CreateFileA);
    gFunctionMap["ReadFile"] = reinterpret_cast<void*>(&stub_ReadFile);
    gFunctionMap["WriteFile"] = reinterpret_cast<void*>(&stub_WriteFile);
    gFunctionMap["SetFilePointer"] = reinterpret_cast<void*>(&stub_SetFilePointer);
    gFunctionMap["SetFilePointerEx"] = reinterpret_cast<void*>(&stub_SetFilePointerEx);
    gFunctionMap["GetFileSize"] = reinterpret_cast<void*>(&stub_GetFileSize);
    gFunctionMap["GetFileSizeEx"] = reinterpret_cast<void*>(&stub_GetFileSizeEx);
    gFunctionMap["GetFileInformationByHandle"] = reinterpret_cast<void*>(&stub_GetFileInformationByHandle);
    gFunctionMap["GetOverlappedResult"] = reinterpret_cast<void*>(&stub_GetOverlappedResult);
    gFunctionMap["CancelIo"] = reinterpret_cast<void*>(&stub_CancelIo);
    gFunctionMap["CancelIoEx"] = reinterpret_cast<void*>(&stub_CancelIoEx);
    gFunctionMap["CloseHandle"] = reinterpret_cast<void*>(&stub_CloseHandle);
    gFunctionMap["FindFirstFileW"] = reinterpret_cast<void*>(&stub_FindFirstFileW);
    gFunctionMap["FindFirstFileExW"] = reinterpret_cast<void*>(&stub_FindFirstFileExW);
    gFunctionMap["FindFirstFileA"] = reinterpret_cast<void*>(&stub_FindFirstFileA);
    gFunctionMap["FindNextFileW"] = reinterpret_cast<void*>(&stub_FindNextFileW);
    gFunctionMap["FindNextFileA"] = reinterpret_cast<void*>(&stub_FindNextFileA);
    gFunctionMap["FindClose"] = reinterpret_cast<void*>(&stub_FindClose);
    gFunctionMap["FlushFileBuffers"] = reinterpret_cast<void*>(&stub_FlushFileBuffers);
    gFunctionMap["SetEndOfFile"] = reinterpret_cast<void*>(&stub_SetEndOfFile);
    gFunctionMap["DeleteFileW"] = reinterpret_cast<void*>(&stub_DeleteFileW);
    gFunctionMap["DeleteFileA"] = reinterpret_cast<void*>(&stub_DeleteFileA);

    // SHELL32 API Registration
    gFunctionMap["SHGetSpecialFolderPathW"] = reinterpret_cast<void*>(&stub_SHGetSpecialFolderPathW);
    gFunctionMap["SHGetKnownFolderPath"] = reinterpret_cast<void*>(&stub_SHGetKnownFolderPath);
    gFunctionMap["SHCreateDirectoryExW"] = reinterpret_cast<void*>(&stub_SHCreateDirectoryExW);
    gFunctionMap["ShellExecuteW"] = reinterpret_cast<void*>(&stub_ShellExecuteW);
    gFunctionMap["DragAcceptFiles"] = reinterpret_cast<void*>(&stub_DragAcceptFiles);

    // USER32 Additional Dialog & Message Registration
    gFunctionMap["MessageBoxA"] = reinterpret_cast<void*>(&stub_MessageBoxA);
    gFunctionMap["MessageBoxW"] = reinterpret_cast<void*>(&stub_MessageBoxW);
    gFunctionMap["GetWindowTextW"] = reinterpret_cast<void*>(&stub_GetWindowTextW);
    gFunctionMap["GetWindowTextLengthW"] = reinterpret_cast<void*>(&stub_GetWindowTextLengthW);
    gFunctionMap["SetWindowTextW"] = reinterpret_cast<void*>(&stub_SetWindowTextW);
    gFunctionMap["SetDlgItemTextW"] = reinterpret_cast<void*>(&stub_SetDlgItemTextW);
    gFunctionMap["GetDlgItemTextW"] = reinterpret_cast<void*>(&stub_GetDlgItemTextW);
    gFunctionMap["SendMessageW"] = reinterpret_cast<void*>(&stub_SendMessageW);
    gFunctionMap["DialogBoxParamW"] = reinterpret_cast<void*>(&stub_DialogBoxParamW);

    // USER32 Window Lifecycle & Focus Registration
    gFunctionMap["GetForegroundWindow"] = reinterpret_cast<void*>(&stub_GetForegroundWindow);
    gFunctionMap["GetActiveWindow"] = reinterpret_cast<void*>(&stub_GetActiveWindow);
    gFunctionMap["GetFocus"] = reinterpret_cast<void*>(&stub_GetFocus);
    gFunctionMap["SetActiveWindow"] = reinterpret_cast<void*>(&stub_SetActiveWindow);
    gFunctionMap["SetForegroundWindow"] = reinterpret_cast<void*>(&stub_SetForegroundWindow);
    gFunctionMap["SetFocus"] = reinterpret_cast<void*>(&stub_SetFocus);
    gFunctionMap["IsIconic"] = reinterpret_cast<void*>(&stub_IsIconic);
    gFunctionMap["IsZoomed"] = reinterpret_cast<void*>(&stub_IsZoomed);
    gFunctionMap["GetCursorPos"] = reinterpret_cast<void*>(&stub_GetCursorPos);
    gFunctionMap["SetCursorPos"] = reinterpret_cast<void*>(&stub_SetCursorPos);
    gFunctionMap["ShowCursor"] = reinterpret_cast<void*>(&stub_ShowCursor);
    gFunctionMap["SetCursor"] = reinterpret_cast<void*>(&stub_SetCursor);
    gFunctionMap["LoadCursorA"] = reinterpret_cast<void*>(&stub_LoadCursorA);
    gFunctionMap["LoadIconA"] = reinterpret_cast<void*>(&stub_LoadIconA);
    gFunctionMap["RegisterClassExA"] = reinterpret_cast<void*>(&stub_RegisterClassExA);
    gFunctionMap["AdjustWindowRectEx"] = reinterpret_cast<void*>(&stub_AdjustWindowRectEx);
    gFunctionMap["SystemParametersInfoA"] = reinterpret_cast<void*>(&stub_SystemParametersInfoA);
    gFunctionMap["MonitorFromWindow"] = reinterpret_cast<void*>(&stub_MonitorFromWindow);
    gFunctionMap["GetMonitorInfoA"] = reinterpret_cast<void*>(&stub_GetMonitorInfoA);
    gFunctionMap["WindowFromPoint"] = reinterpret_cast<void*>(&stub_WindowFromPoint);
    gFunctionMap["ScreenToClient"] = reinterpret_cast<void*>(&stub_ScreenToClient);
    gFunctionMap["ClientToScreen"] = reinterpret_cast<void*>(&stub_ClientToScreen);
    gFunctionMap["TrackMouseEvent"] = reinterpret_cast<void*>(&stub_TrackMouseEvent);
    gFunctionMap["SetTimer"] = reinterpret_cast<void*>(&stub_SetTimer);
    gFunctionMap["KillTimer"] = reinterpret_cast<void*>(&stub_KillTimer);
    gFunctionMap["GetKeyState"] = reinterpret_cast<void*>(&stub_GetKeyState);
    gFunctionMap["GetKeyboardState"] = reinterpret_cast<void*>(&stub_GetKeyboardState);
    gFunctionMap["AttachThreadInput"] = reinterpret_cast<void*>(&stub_AttachThreadInput);
    gFunctionMap["GetWindowLongA"] = reinterpret_cast<void*>(&stub_GetWindowLongA);
    gFunctionMap["SetWindowLongA"] = reinterpret_cast<void*>(&stub_SetWindowLongA);
    gFunctionMap["GetWindowLongPtrA"] = reinterpret_cast<void*>(&stub_GetWindowLongPtrA);
    gFunctionMap["SetWindowLongPtrA"] = reinterpret_cast<void*>(&stub_SetWindowLongPtrA);
    gFunctionMap["GetWindowPlacement"] = reinterpret_cast<void*>(&stub_GetWindowPlacement);
    gFunctionMap["InvalidateRect"] = reinterpret_cast<void*>(&stub_InvalidateRect);
    gFunctionMap["SendMessageA"] = reinterpret_cast<void*>(&stub_SendMessageA);
    gFunctionMap["CallWindowProcA"] = reinterpret_cast<void*>(&stub_CallWindowProcA);
    gFunctionMap["EndDialog"] = reinterpret_cast<void*>(&stub_EndDialog);
    gFunctionMap["GetDlgItem"] = reinterpret_cast<void*>(&stub_GetDlgItem);

    // Stack & Exception Handling Registration
    gFunctionMap["RtlPcToFileHeader"] = reinterpret_cast<void*>(&stub_RtlPcToFileHeader);
    gFunctionMap["RaiseException"] = reinterpret_cast<void*>(&stub_RaiseException);
    gFunctionMap["RtlUnwind"] = reinterpret_cast<void*>(&stub_RtlUnwind);
    gFunctionMap["RtlUnwindEx"] = reinterpret_cast<void*>(&stub_RtlUnwindEx);

    // D3D Compiler Registration
    gFunctionMap["D3DReflect"] = reinterpret_cast<void*>(&D3DReflect);

    // DirectInput Registration
    gFunctionMap["DirectInput8Create"] = reinterpret_cast<void*>(&DirectInput8Create);

    // XAudio2 Audio Subsystem Registration
    gFunctionMap["XAudio2Create"] = reinterpret_cast<void*>(&XAudio2Create);

    // Threading Registration
    gFunctionMap["CreateThread"] = reinterpret_cast<void*>(&stub_CreateThread);
    gFunctionMap["ResumeThread"] = reinterpret_cast<void*>(&stub_ResumeThread);
    gFunctionMap["SetThreadPriority"] = reinterpret_cast<void*>(&stub_SetThreadPriority);
    gFunctionMap["SetThreadIdealProcessor"] = reinterpret_cast<void*>(&stub_SetThreadIdealProcessor);
    gFunctionMap["GetExitCodeThread"] = reinterpret_cast<void*>(&stub_GetExitCodeThread);
    gFunctionMap["TerminateThread"] = reinterpret_cast<void*>(&stub_TerminateThread);
    gFunctionMap["ExitThread"] = reinterpret_cast<void*>(&stub_ExitThread);
    gFunctionMap["FreeLibraryAndExitThread"] = reinterpret_cast<void*>(&stub_FreeLibraryAndExitThread);
    gFunctionMap["GetProcessAffinityMask"] = reinterpret_cast<void*>(&stub_GetProcessAffinityMask);

    // ADVAPI32 Crypto Registration
    gFunctionMap["CryptAcquireContextA"] = reinterpret_cast<void*>(&stub_CryptAcquireContextA);
    gFunctionMap["CryptReleaseContext"] = reinterpret_cast<void*>(&stub_CryptReleaseContext);
    gFunctionMap["CryptImportKey"] = reinterpret_cast<void*>(&stub_CryptImportKey);
    gFunctionMap["CryptSetKeyParam"] = reinterpret_cast<void*>(&stub_CryptSetKeyParam);
    gFunctionMap["CryptEncrypt"] = reinterpret_cast<void*>(&stub_CryptEncrypt);
    gFunctionMap["CryptDecrypt"] = reinterpret_cast<void*>(&stub_CryptDecrypt);
    gFunctionMap["CryptDestroyKey"] = reinterpret_cast<void*>(&stub_CryptDestroyKey);
    gFunctionMap["CryptDestroyHash"] = reinterpret_cast<void*>(&stub_CryptDestroyHash);
    gFunctionMap["CryptGenRandom"] = reinterpret_cast<void*>(&stub_CryptGenRandom);
    gFunctionMap["SystemFunction036"] = reinterpret_cast<void*>(&stub_SystemFunction036);
    gFunctionMap["RtlGenRandom"] = reinterpret_cast<void*>(&stub_SystemFunction036);

    // GDI32 Registration
    gFunctionMap["GetDeviceCaps"] = reinterpret_cast<void*>(&stub_GetDeviceCaps);
    gFunctionMap["GetStockObject"] = reinterpret_cast<void*>(&stub_GetStockObject);

    // WINMM Registration
    gFunctionMap["timeBeginPeriod"] = reinterpret_cast<void*>(&stub_timeBeginPeriod);
    gFunctionMap["timeEndPeriod"] = reinterpret_cast<void*>(&stub_timeEndPeriod);
    gFunctionMap["PlaySoundA"] = reinterpret_cast<void*>(&stub_PlaySoundA);

    // OLE32 Registration
    gFunctionMap["CoInitializeEx"] = reinterpret_cast<void*>(&stub_CoInitializeEx);
    gFunctionMap["CoUninitialize"] = reinterpret_cast<void*>(&stub_CoUninitialize);
    gFunctionMap["CoCreateGuid"] = reinterpret_cast<void*>(&stub_CoCreateGuid);
    gFunctionMap["CoCreateInstance"] = reinterpret_cast<void*>(&stub_CoCreateInstance);
    gFunctionMap["CoTaskMemAlloc"] = reinterpret_cast<void*>(&stub_CoTaskMemAlloc);
    gFunctionMap["CoTaskMemFree"] = reinterpret_cast<void*>(&stub_CoTaskMemFree);
    gFunctionMap["PropVariantClear"] = reinterpret_cast<void*>(&stub_PropVariantClear);

    // KERNEL32 Misc, Resource & Locale Registration
    gFunctionMap["GetCPInfo"] = reinterpret_cast<void*>(&stub_GetCPInfo);
    gFunctionMap["GetLocaleInfoW"] = reinterpret_cast<void*>(&stub_GetLocaleInfoW);
    gFunctionMap["GetUserDefaultLCID"] = reinterpret_cast<void*>(&stub_GetUserDefaultLCID);
    gFunctionMap["IsValidLocale"] = reinterpret_cast<void*>(&stub_IsValidLocale);
    gFunctionMap["EnumSystemLocalesW"] = reinterpret_cast<void*>(&stub_EnumSystemLocalesW);
    gFunctionMap["CompareStringW"] = reinterpret_cast<void*>(&stub_CompareStringW);
    gFunctionMap["LCMapStringW"] = reinterpret_cast<void*>(&stub_LCMapStringW);
    gFunctionMap["LCMapStringEx"] = reinterpret_cast<void*>(&stub_LCMapStringEx);
    gFunctionMap["GetStringTypeW"] = reinterpret_cast<void*>(&stub_GetStringTypeW);
    gFunctionMap["GetTimeZoneInformation"] = reinterpret_cast<void*>(&stub_GetTimeZoneInformation);
    gFunctionMap["GetVersionExA"] = reinterpret_cast<void*>(&stub_GetVersionExA);
    gFunctionMap["SetEnvironmentVariableW"] = reinterpret_cast<void*>(&stub_SetEnvironmentVariableW);
    gFunctionMap["FormatMessageA"] = reinterpret_cast<void*>(&stub_FormatMessageA);
    gFunctionMap["FindResourceA"] = reinterpret_cast<void*>(&stub_FindResourceA);
    gFunctionMap["LoadResource"] = reinterpret_cast<void*>(&stub_LoadResource);
    gFunctionMap["LockResource"] = reinterpret_cast<void*>(&stub_LockResource);
    gFunctionMap["SizeofResource"] = reinterpret_cast<void*>(&stub_SizeofResource);
    gFunctionMap["FreeResource"] = reinterpret_cast<void*>(&stub_FreeResource);

    // USER32 Window Properties, Monitors & Thread Process ID Registration
    gFunctionMap["GetWindowThreadProcessId"] = reinterpret_cast<void*>(&stub_GetWindowThreadProcessId);
    gFunctionMap["SetClassLongPtrA"] = reinterpret_cast<void*>(&stub_SetClassLongPtrA);
    gFunctionMap["FindWindowA"] = reinterpret_cast<void*>(&stub_FindWindowA);
    gFunctionMap["SetPropA"] = reinterpret_cast<void*>(&stub_SetPropA);
    gFunctionMap["GetPropA"] = reinterpret_cast<void*>(&stub_GetPropA);
    gFunctionMap["RemovePropA"] = reinterpret_cast<void*>(&stub_RemovePropA);
    gFunctionMap["EnumDisplayMonitors"] = reinterpret_cast<void*>(&stub_EnumDisplayMonitors);
    gFunctionMap["MonitorFromRect"] = reinterpret_cast<void*>(&stub_MonitorFromRect);

    // Network & Hardware Identification Registration
    gFunctionMap["GetAdaptersInfo"] = reinterpret_cast<void*>(&stub_GetAdaptersInfo);
    gFunctionMap["Netbios"] = reinterpret_cast<void*>(&stub_Netbios);
    gFunctionMap["PropVariantToInt64"] = reinterpret_cast<void*>(&stub_PropVariantToInt64);

    // Media Foundation (MFPlat.DLL & MFReadWrite.dll) Registration
    gFunctionMap["MFStartup"] = reinterpret_cast<void*>(&stub_MFStartup);
    gFunctionMap["MFShutdown"] = reinterpret_cast<void*>(&stub_MFShutdown);
    gFunctionMap["MFCreateAttributes"] = reinterpret_cast<void*>(&stub_MFCreateAttributes);
    gFunctionMap["MFCreateMediaType"] = reinterpret_cast<void*>(&stub_MFCreateMediaType);
    gFunctionMap["MFCreateDXGIDeviceManager"] = reinterpret_cast<void*>(&stub_MFCreateDXGIDeviceManager);
    gFunctionMap["MFCreateMemoryBuffer"] = reinterpret_cast<void*>(&stub_MFCreateMemoryBuffer);
    gFunctionMap["MFCreateSample"] = reinterpret_cast<void*>(&stub_MFCreateSample);
    gFunctionMap["MFCalculateImageSize"] = reinterpret_cast<void*>(&stub_MFCalculateImageSize);
    gFunctionMap["MFGetStrideForBitmapInfoHeader"] = reinterpret_cast<void*>(&stub_MFGetStrideForBitmapInfoHeader);
    gFunctionMap["MFllMulDiv"] = reinterpret_cast<void*>(&stub_MFllMulDiv);
    gFunctionMap["MFCreateSourceResolver"] = reinterpret_cast<void*>(&stub_MFCreateSourceResolver);
    gFunctionMap["MFCreateAsyncResult"] = reinterpret_cast<void*>(&stub_MFCreateAsyncResult);
    gFunctionMap["MFInvokeCallback"] = reinterpret_cast<void*>(&stub_MFInvokeCallback);
    gFunctionMap["MFCreateSourceReaderFromMediaSource"] = reinterpret_cast<void*>(&stub_MFCreateSourceReaderFromMediaSource);
    gFunctionMap["MFCreateSourceReaderFromURL"] = reinterpret_cast<void*>(&stub_MFCreateSourceReaderFromURL);

    // WinMM (Wave Audio) Registration
    gFunctionMap["waveOutGetNumDevs"] = reinterpret_cast<void*>(&stub_waveOutGetNumDevs);
    gFunctionMap["waveOutGetDevCapsA"] = reinterpret_cast<void*>(&stub_waveOutGetDevCapsA);
    gFunctionMap["waveOutOpen"] = reinterpret_cast<void*>(&stub_waveOutOpen);
    gFunctionMap["waveOutClose"] = reinterpret_cast<void*>(&stub_waveOutClose);
    gFunctionMap["waveOutPrepareHeader"] = reinterpret_cast<void*>(&stub_waveOutPrepareHeader);
    gFunctionMap["waveOutUnprepareHeader"] = reinterpret_cast<void*>(&stub_waveOutUnprepareHeader);
    gFunctionMap["waveOutWrite"] = reinterpret_cast<void*>(&stub_waveOutWrite);
    gFunctionMap["waveOutReset"] = reinterpret_cast<void*>(&stub_waveOutReset);
    gFunctionMap["waveOutPause"] = reinterpret_cast<void*>(&stub_waveOutPause);
    gFunctionMap["waveOutRestart"] = reinterpret_cast<void*>(&stub_waveOutRestart);
    gFunctionMap["waveOutGetPosition"] = reinterpret_cast<void*>(&stub_waveOutGetPosition);

    gFunctionMap["waveInGetNumDevs"] = reinterpret_cast<void*>(&stub_waveInGetNumDevs);
    gFunctionMap["waveInGetDevCapsA"] = reinterpret_cast<void*>(&stub_waveInGetDevCapsA);
    gFunctionMap["waveInOpen"] = reinterpret_cast<void*>(&stub_waveInOpen);
    gFunctionMap["waveInClose"] = reinterpret_cast<void*>(&stub_waveInClose);
    gFunctionMap["waveInPrepareHeader"] = reinterpret_cast<void*>(&stub_waveInPrepareHeader);
    gFunctionMap["waveInUnprepareHeader"] = reinterpret_cast<void*>(&stub_waveInUnprepareHeader);
    gFunctionMap["waveInAddBuffer"] = reinterpret_cast<void*>(&stub_waveInAddBuffer);
    gFunctionMap["waveInStart"] = reinterpret_cast<void*>(&stub_waveInStart);
    gFunctionMap["waveInStop"] = reinterpret_cast<void*>(&stub_waveInStop);
    gFunctionMap["waveInReset"] = reinterpret_cast<void*>(&stub_waveInReset);

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

    // Fallback cho IAT resolving tĩnh: Cảnh báo và trả về stub_GenericSuccess để tránh crash null-pointer
    LOGW("Win32Bridge_ResolveSymbol: CẢNH BÁO: API chưa triển khai: [%s]::%s -> fallback stub_GenericSuccess",
         dllName, funcName);
    return reinterpret_cast<void*>(&stub_GenericSuccess);
}

extern "C" void Win32Bridge_PostMessage(uint32_t message, uint64_t wParam, int64_t lParam) {
    std::lock_guard<std::mutex> lock(gMsgMutex);
    Win32Msg msg{};
    msg.hwnd = DUMMY_WINDOW;
    msg.message = message;
    msg.wParam = wParam;
    msg.lParam = lParam;
    msg.time = stub_GetTickCount();
    msg.pt.x = static_cast<int32_t>(lParam & 0xFFFF);
    msg.pt.y = static_cast<int32_t>((lParam >> 16) & 0xFFFF);
    msg.lPrivate = 0;
    gMsgQueue.push(msg);
}

extern "C" void Win32Bridge_SetKeyState(uint8_t vkCode, bool isDown) {
    gVirtualKeyState[vkCode].store(isDown ? 1 : 0);
}

extern "C" bool Win32Bridge_GetKeyState(uint8_t vkCode) {
    return gVirtualKeyState[vkCode].load() != 0;
}

extern "C" void Win32Bridge_SetCursorPos(int32_t x, int32_t y) {
    gCursorX.store(x);
    gCursorY.store(y);
}
