#include "win32_bridge.h"
#include "d3d11_bridge.h"
#include "dinput_bridge.h"
#include "xaudio2_bridge.h"
#include <android/log.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
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
#include <wchar.h>

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

// String conversions
int32_t WINAPI stub_MultiByteToWideChar(uint32_t, uint32_t, const char* lpMultiByteStr, int32_t cbMultiByte, wchar_t* lpWideCharStr, int32_t cchWideChar) {
    if (!lpMultiByteStr) return 0;
    int32_t len = cbMultiByte >= 0 ? cbMultiByte : static_cast<int32_t>(strlen(lpMultiByteStr));
    if (cchWideChar == 0) {
        return len + (cbMultiByte < 0 ? 1 : 0);
    }
    if (!lpWideCharStr) return 0;
    int32_t count = 0;
    for (int32_t i = 0; i < len && count < cchWideChar; ++i) {
        lpWideCharStr[count++] = static_cast<wchar_t>(static_cast<unsigned char>(lpMultiByteStr[i]));
    }
    if (cbMultiByte < 0 && count < cchWideChar) {
        lpWideCharStr[count++] = L'\0';
    }
    return count;
}

int32_t WINAPI stub_WideCharToMultiByte(uint32_t, uint32_t, const wchar_t* lpWideCharStr, int32_t cchWideChar, char* lpMultiByteStr, int32_t cbMultiByte, const char*, int32_t*) {
    if (!lpWideCharStr) return 0;
    int32_t len = cchWideChar >= 0 ? cchWideChar : static_cast<int32_t>(wcslen(lpWideCharStr));
    if (cbMultiByte == 0) {
        return len + (cchWideChar < 0 ? 1 : 0);
    }
    if (!lpMultiByteStr) return 0;
    int32_t count = 0;
    for (int32_t i = 0; i < len && count < cbMultiByte; ++i) {
        lpMultiByteStr[count++] = static_cast<char>(lpWideCharStr[i] & 0x7F);
    }
    if (cchWideChar < 0 && count < cbMultiByte) {
        lpMultiByteStr[count++] = '\0';
    }
    return count;
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

// Synchronization & Events
void* WINAPI stub_CreateEventA(void*, int32_t, int32_t, const char*) { return reinterpret_cast<void*>(0x50005001); }
void* WINAPI stub_CreateEventExA(void*, const char*, uint32_t, uint32_t) { return reinterpret_cast<void*>(0x50005002); }
int32_t WINAPI stub_SetEvent(void*) { return 1; }
int32_t WINAPI stub_ResetEvent(void*) { return 1; }
void* WINAPI stub_CreateSemaphoreA(void*, int32_t, int32_t, const char*) { return reinterpret_cast<void*>(0x50005003); }
void* WINAPI stub_CreateSemaphoreW(void*, int32_t, int32_t, const wchar_t*) { return reinterpret_cast<void*>(0x50005004); }
int32_t WINAPI stub_ReleaseSemaphore(void*, int32_t, int32_t*) { return 1; }
uint32_t WINAPI stub_WaitForSingleObject(void*, uint32_t) { return 0; }
uint32_t WINAPI stub_WaitForSingleObjectEx(void*, uint32_t, int32_t) { return 0; }
int32_t WINAPI stub_SwitchToThread() { sched_yield(); return 1; }

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
    wchar_t cFileName[260];
    wchar_t cAlternateFileName[14];
};

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

struct VfsFileHandle {
    int fd;
    std::string path;
    uint32_t accessMode;
};

struct VfsFindState {
    DIR* dir;
    std::string pattern;
    std::string dirPath;
};

static std::string gVfsDataDir = "/storage/emulated/0/Android/data/com.makurisu.sgrb/files/data";
static std::mutex gVfsMutex;
static std::unordered_map<uintptr_t, VfsFileHandle> gVfsHandles;
static std::unordered_map<uintptr_t, VfsFindState> gVfsFindStates;
static uintptr_t gNextHandleId = 0x10000;

static std::string normalizeWinPathW(const wchar_t* wpath) {
    if (!wpath) return "";
    std::string s;
    for (size_t i = 0; wpath[i] != 0; ++i) {
        wchar_t wc = wpath[i];
        if (wc == L'\\') s.push_back('/');
        else if (wc < 128) s.push_back(static_cast<char>(wc));
        else {
            s.push_back(static_cast<char>(wc & 0xFF));
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

// Tìm kiếm đường dẫn theo cơ chế Case-Insensitive (vì Windows không phân biệt hoa thường, còn Android Linux thì có)
static std::string resolveCaseInsensitivePath(const std::string& rootDir, const std::string& relPath) {
    if (relPath.empty()) return rootDir;

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < relPath.size()) {
        size_t end = relPath.find('/', start);
        if (end == std::string::npos) end = relPath.size();
        if (end > start) {
            std::string part = relPath.substr(start, end - start);
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
            return currentPath;
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
    const wchar_t* lpFileName,
    uint32_t dwDesiredAccess,
    uint32_t /* dwShareMode */,
    void* /* lpSecurityAttributes */,
    uint32_t dwCreationDisposition,
    uint32_t /* dwFlagsAndAttributes */,
    void* /* hTemplateFile */
) {
    if (!lpFileName) return reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    std::string relPath = normalizeWinPathW(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    LOGI("CreateFileW: '%ls' -> mapped: '%s', disp=%u, access=0x%X",
         lpFileName, fullPath.c_str(), dwCreationDisposition, dwDesiredAccess);

    int flags = 0;
    bool isWrite = (dwDesiredAccess & 0x40000000) != 0;
    bool isRead = (dwDesiredAccess & 0x80000000) != 0;

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
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    uintptr_t handleId = ++gNextHandleId;
    gVfsHandles[handleId] = { fd, fullPath, dwDesiredAccess };
    LOGI("-> Mở file thành công! Handle=%p, fd=%d", reinterpret_cast<void*>(handleId), fd);
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
    if (!lpFileName) return reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    std::string relPath = normalizeWinPathA(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    LOGI("CreateFileA: '%s' -> mapped: '%s', disp=%u, access=0x%X",
         lpFileName, fullPath.c_str(), dwCreationDisposition, dwDesiredAccess);

    int flags = 0;
    bool isWrite = (dwDesiredAccess & 0x40000000) != 0;
    bool isRead = (dwDesiredAccess & 0x80000000) != 0;

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
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    uintptr_t handleId = ++gNextHandleId;
    gVfsHandles[handleId] = { fd, fullPath, dwDesiredAccess };
    LOGI("-> Mở file thành công! Handle=%p, fd=%d", reinterpret_cast<void*>(handleId), fd);
    return reinterpret_cast<void*>(handleId);
}

int32_t WINAPI stub_ReadFile(
    void* hFile,
    void* lpBuffer,
    uint32_t nNumberOfBytesToRead,
    uint32_t* lpNumberOfBytesRead,
    void* /* lpOverlapped */
) {
    if (!hFile || hFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1)) || !lpBuffer) {
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return 0;
    }

    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hFile));
        if (it != gVfsHandles.end()) fd = it->second.fd;
    }

    if (fd < 0) {
        LOGW("ReadFile: Handle không hợp lệ: %p", hFile);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return 0;
    }

    ssize_t bytesRead = read(fd, lpBuffer, nNumberOfBytesToRead);
    if (bytesRead < 0) {
        LOGW("ReadFile: Lỗi đọc fd=%d (errno=%d)", fd, errno);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return 0;
    }

    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<uint32_t>(bytesRead);
    return 1;
}

int32_t WINAPI stub_WriteFile(
    void* hFile,
    const void* lpBuffer,
    uint32_t nNumberOfBytesToWrite,
    uint32_t* lpNumberOfBytesWritten,
    void* /* lpOverlapped */
) {
    if (!hFile || hFile == reinterpret_cast<void*>(static_cast<intptr_t>(-1)) || !lpBuffer) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
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
        return 0;
    }

    ssize_t bytesWritten = write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (bytesWritten < 0) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return 0;
    }

    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<uint32_t>(bytesWritten);
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

int32_t WINAPI stub_CloseHandle(void* hObject) {
    if (!hObject || hObject == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(gVfsMutex);
    auto it = gVfsHandles.find(reinterpret_cast<uintptr_t>(hObject));
    if (it != gVfsHandles.end()) {
        close(it->second.fd);
        gVfsHandles.erase(it);
        return 1;
    }

    auto fit = gVfsFindStates.find(reinterpret_cast<uintptr_t>(hObject));
    if (fit != gVfsFindStates.end()) {
        if (fit->second.dir) closedir(fit->second.dir);
        gVfsFindStates.erase(fit);
        return 1;
    }

    return 1;
}

uint32_t WINAPI stub_GetFileAttributesW(const wchar_t* lpFileName) {
    if (!lpFileName) return 0xFFFFFFFF;

    std::string relPath = normalizeWinPathW(lpFileName);
    std::string fullPath = resolveCaseInsensitivePath(gVfsDataDir, relPath);

    struct stat64 st;
    if (stat64(fullPath.c_str(), &st) != 0) {
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
        return 0xFFFFFFFF;
    }

    if (S_ISDIR(st.st_mode)) return 0x10;
    return 0x80;
}

void* WINAPI stub_FindFirstFileW(const wchar_t* lpFileName, void* lpFindFileData) {
    if (!lpFileName || !lpFindFileData) return reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    std::string normPath = normalizeWinPathW(lpFileName);
    size_t lastSlash = normPath.rfind('/');
    std::string dirPart = (lastSlash != std::string::npos) ? normPath.substr(0, lastSlash) : "";
    std::string patPart = (lastSlash != std::string::npos) ? normPath.substr(lastSlash + 1) : normPath;

    std::string fullDirPath = resolveCaseInsensitivePath(gVfsDataDir, dirPart);
    DIR* dir = opendir(fullDirPath.c_str());
    if (!dir) return reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    auto* findData = reinterpret_cast<Win32FindDataW*>(lpFindFileData);
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        bool match = (patPart == "*" || patPart == "*.*");
        if (!match) {
            if (patPart.size() > 2 && patPart[0] == '*' && patPart[1] == '.') {
                std::string ext = patPart.substr(1);
                std::string name = entry->d_name;
                if (name.size() >= ext.size() &&
                    strcasecmp(name.c_str() + name.size() - ext.size(), ext.c_str()) == 0) {
                    match = true;
                }
            } else if (strcasecmp(entry->d_name, patPart.c_str()) == 0) {
                match = true;
            }
        }

        if (match) {
            memset(findData, 0, sizeof(Win32FindDataW));
            std::string itemFullPath = fullDirPath + "/" + entry->d_name;
            struct stat64 st;
            if (stat64(itemFullPath.c_str(), &st) == 0) {
                findData->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
                findData->nFileSizeHigh = static_cast<uint32_t>(st.st_size >> 32);
                findData->nFileSizeLow = static_cast<uint32_t>(st.st_size & 0xFFFFFFFF);
            } else {
                findData->dwFileAttributes = 0x80;
            }
            for (size_t k = 0; k < 259 && entry->d_name[k] != 0; ++k) {
                findData->cFileName[k] = static_cast<wchar_t>(static_cast<uint8_t>(entry->d_name[k]));
            }

            std::lock_guard<std::mutex> lock(gVfsMutex);
            uintptr_t findId = ++gNextHandleId;
            gVfsFindStates[findId] = { dir, patPart, fullDirPath };
            return reinterpret_cast<void*>(findId);
        }
    }

    closedir(dir);
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

int32_t WINAPI stub_FindNextFileW(void* hFindFile, void* lpFindFileData) {
    if (!hFindFile || !lpFindFileData) return 0;

    DIR* dir = nullptr;
    std::string patPart;
    std::string fullDirPath;
    {
        std::lock_guard<std::mutex> lock(gVfsMutex);
        auto it = gVfsFindStates.find(reinterpret_cast<uintptr_t>(hFindFile));
        if (it != gVfsFindStates.end()) {
            dir = it->second.dir;
            patPart = it->second.pattern;
            fullDirPath = it->second.dirPath;
        }
    }
    if (!dir) return 0;

    auto* findData = reinterpret_cast<Win32FindDataW*>(lpFindFileData);
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        bool match = (patPart == "*" || patPart == "*.*");
        if (!match) {
            if (patPart.size() > 2 && patPart[0] == '*' && patPart[1] == '.') {
                std::string ext = patPart.substr(1);
                std::string name = entry->d_name;
                if (name.size() >= ext.size() &&
                    strcasecmp(name.c_str() + name.size() - ext.size(), ext.c_str()) == 0) {
                    match = true;
                }
            } else if (strcasecmp(entry->d_name, patPart.c_str()) == 0) {
                match = true;
            }
        }

        if (match) {
            memset(findData, 0, sizeof(Win32FindDataW));
            std::string itemFullPath = fullDirPath + "/" + entry->d_name;
            struct stat64 st;
            if (stat64(itemFullPath.c_str(), &st) == 0) {
                findData->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
                findData->nFileSizeHigh = static_cast<uint32_t>(st.st_size >> 32);
                findData->nFileSizeLow = static_cast<uint32_t>(st.st_size & 0xFFFFFFFF);
            } else {
                findData->dwFileAttributes = 0x80;
            }
            for (size_t k = 0; k < 259 && entry->d_name[k] != 0; ++k) {
                findData->cFileName[k] = static_cast<wchar_t>(static_cast<uint8_t>(entry->d_name[k]));
            }
            return 1;
        }
    }
    return 0;
}

int32_t WINAPI stub_FindClose(void* hFindFile) {
    if (!hFindFile) return 1;
    std::lock_guard<std::mutex> lock(gVfsMutex);
    auto it = gVfsFindStates.find(reinterpret_cast<uintptr_t>(hFindFile));
    if (it != gVfsFindStates.end()) {
        if (it->second.dir) closedir(it->second.dir);
        gVfsFindStates.erase(it);
    }
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

int32_t WINAPI stub_DeleteFileW(const wchar_t* lpFileName) {
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

int32_t WINAPI stub_GetCurrentDirectoryW(uint32_t nBufferLength, wchar_t* lpBuffer) {
    if (lpBuffer && nBufferLength > 3) {
        lpBuffer[0] = L'C'; lpBuffer[1] = L':'; lpBuffer[2] = L'\\'; lpBuffer[3] = L'\0';
    }
    return 3;
}

int32_t WINAPI stub_SetCurrentDirectoryW(const wchar_t*) { return 1; }
uint32_t WINAPI stub_GetModuleFileNameA(void*, char* lpFilename, uint32_t nSize) {
    if (lpFilename && nSize > 14) {
        strcpy(lpFilename, "sgre_steam.exe");
        return 14;
    }
    return 0;
}
uint32_t WINAPI stub_GetModuleFileNameW(void*, wchar_t* lpFilename, uint32_t nSize) {
    if (lpFilename && nSize > 14) {
        wcscpy(lpFilename, L"sgre_steam.exe");
        return 14;
    }
    return 0;
}

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
void* WINAPI stub_RtlPcToFileHeader(void*, void** baseOfImage) {
    if (baseOfImage) *baseOfImage = gLoadedImageBase;
    return gLoadedImageBase;
}
void WINAPI stub_RaiseException(uint32_t, uint32_t, uint32_t, const uint64_t*) {}
void WINAPI stub_RtlUnwind(void*, void*, void*, void*) {}
void WINAPI stub_RtlUnwindEx(void*, void*, void*, void*, void*, void*) {}
void WINAPI stub_TerminateProcess(void*, uint32_t uExitCode) {
    LOGW("stub_TerminateProcess gọi với mã thoát: %u", uExitCode);
    pthread_exit(nullptr);
}
void WINAPI stub_ExitProcess(uint32_t uExitCode) {
    LOGW("stub_ExitProcess gọi với mã thoát: %u", uExitCode);
    pthread_exit(nullptr);
}

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

extern "C" void Win32Bridge_SetLoadedImageBase(void* imageBase) {
    gLoadedImageBase = imageBase;
    LOGI("Win32Bridge đã thiết lập Loaded ImageBase: %p", imageBase);
}

extern "C" void Win32Bridge_SetDataDirectory(const char* dataDir) {
    if (dataDir && dataDir[0] != '\0') {
        gVfsDataDir = dataDir;
        LOGI("Win32Bridge VFS: Gán thư mục dữ liệu gốc = '%s'", gVfsDataDir.c_str());
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
    gFunctionMap["CreateEventExA"] = reinterpret_cast<void*>(&stub_CreateEventExA);
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
    gFunctionMap["SetCurrentDirectoryW"] = reinterpret_cast<void*>(&stub_SetCurrentDirectoryW);
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
    gFunctionMap["CloseHandle"] = reinterpret_cast<void*>(&stub_CloseHandle);
    gFunctionMap["FindFirstFileW"] = reinterpret_cast<void*>(&stub_FindFirstFileW);
    gFunctionMap["FindNextFileW"] = reinterpret_cast<void*>(&stub_FindNextFileW);
    gFunctionMap["FindClose"] = reinterpret_cast<void*>(&stub_FindClose);
    gFunctionMap["FlushFileBuffers"] = reinterpret_cast<void*>(&stub_FlushFileBuffers);
    gFunctionMap["SetEndOfFile"] = reinterpret_cast<void*>(&stub_SetEndOfFile);
    gFunctionMap["DeleteFileW"] = reinterpret_cast<void*>(&stub_DeleteFileW);
    gFunctionMap["DeleteFileA"] = reinterpret_cast<void*>(&stub_DeleteFileA);

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
