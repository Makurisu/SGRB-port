#include "arm64_box64_bridge.h"
#include "win32_bridge.h"
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <cstring>
#include <mutex>
#include <vector>

#define LOG_TAG "SGRE_Box64Bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace sgre {

namespace {

constexpr size_t kThunkSlotBytes = 16u;
constexpr size_t kMaxHostThunks = 4096u;
constexpr size_t kThunkArenaBytes = kThunkSlotBytes * kMaxHostThunks;

struct ThunkEntry {
    const char* debugName = nullptr;
    void* nativeTarget = nullptr;
    HostThunkHandler marshaller = nullptr;
    void* x64StubAddr = nullptr;
};

using FnBox64Init = int (*)(int argc, const char** argv, char** envp);
using FnBox64RegisterHostBridge = void (*)(bool (*dispatcher)(uint32_t thunkId, WinX64CallContext* ctx));
using FnBox64RunX64Entry = uint64_t (*)(uintptr_t entryPointVa, uintptr_t stackTop);
using FnBox64EmuRun = void (*)(void* emuContext);

struct Box64DynamicSymbols {
    void* dlHandle = nullptr;
    FnBox64Init fnInit = nullptr;
    FnBox64RegisterHostBridge fnRegisterBridge = nullptr;
    FnBox64RunX64Entry fnRunEntry = nullptr;
    FnBox64EmuRun fnDynaRun = nullptr;
    Box64RuntimeStatus status = Box64RuntimeStatus::MissingRuntimeLibrary;
};

static std::mutex gThunkMutex;
static uint8_t* gThunkCodeArena = nullptr;
static std::vector<ThunkEntry> gThunkTable;
static Box64DynamicSymbols gBox64Syms;

static bool ensureThunkArenaLocked() {
    if (gThunkCodeArena != nullptr) {
        return true;
    }
    void* mapped = ::mmap(
        nullptr,
        kThunkArenaBytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (mapped == MAP_FAILED || mapped == nullptr) {
        LOGE("Không thể cấp phát vùng nhớ Host Thunk Arena (%zu bytes)", kThunkArenaBytes);
        return false;
    }
    gThunkCodeArena = static_cast<uint8_t*>(mapped);
    std::memset(gThunkCodeArena, 0xCC, kThunkArenaBytes); // int3 padding an toàn
    gThunkTable.reserve(512);
    return true;
}

// Mẫu stub x86_64 16-byte chuẩn cho Box64 Host Escape:
//   [0..1]   0F 3F          ; Custom Box64 Host-Bridge Escape Opcode
//   [2..5]   thunkId (u32)  ; Chỉ số hàm host trong gThunkTable
//   [6]      C3             ; ret (trở về hàm gọi trong sgre_steam.exe)
//   [7..15]  CC ...         ; căn lề 16-byte
static void writeX64HostEscapeStub(uint8_t* slotPtr, uint32_t thunkId) {
    slotPtr[0] = 0x0F;
    slotPtr[1] = 0x3F;
    std::memcpy(slotPtr + 2, &thunkId, sizeof(uint32_t));
    slotPtr[6] = 0xC3;
    std::memset(slotPtr + 7, 0xCC, kThunkSlotBytes - 7u);
}

static bool cBridgeCallbackAdapter(uint32_t thunkId, WinX64CallContext* ctx) {
    if (!ctx) return false;
    return Arm64Box64Bridge::dispatchHostThunk(thunkId, *ctx);
}

static Box64RuntimeStatus loadBox64SymbolsLocked(const std::string& dataDirectory) {
#if defined(__x86_64__) || defined(_M_X64)
    (void)dataDirectory;
    gBox64Syms.status = Box64RuntimeStatus::NativeX86_64;
    return gBox64Syms.status;
#else
    if (gBox64Syms.dlHandle != nullptr && gBox64Syms.status == Box64RuntimeStatus::Ready) {
        return Box64RuntimeStatus::Ready;
    }

    std::vector<std::string> candidatePaths;
    if (!dataDirectory.empty()) {
        candidatePaths.push_back(dataDirectory + "/box64/libbox64.so");
        candidatePaths.push_back(dataDirectory + "/libbox64.so");
    }
    const char* envLibPath = std::getenv("SGRB_BOX64_PATH");
    if (envLibPath && envLibPath[0] != '\0') {
        candidatePaths.insert(candidatePaths.begin(), std::string(envLibPath));
    }
    candidatePaths.push_back("libbox64_sgrb.so");
    candidatePaths.push_back("libbox64.so");

    void* handle = nullptr;
    std::string loadedPath;
    for (const auto& path : candidatePaths) {
        handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr) {
            loadedPath = path;
            break;
        }
    }

    if (handle == nullptr) {
        gBox64Syms.status = Box64RuntimeStatus::MissingRuntimeLibrary;
        return gBox64Syms.status;
    }

    gBox64Syms.dlHandle = handle;
    gBox64Syms.fnInit = reinterpret_cast<FnBox64Init>(::dlsym(handle, "Box64_Initialize"));
    if (!gBox64Syms.fnInit) {
        gBox64Syms.fnInit = reinterpret_cast<FnBox64Init>(::dlsym(handle, "InitializeBox64"));
    }

    gBox64Syms.fnRegisterBridge = reinterpret_cast<FnBox64RegisterHostBridge>(
        ::dlsym(handle, "Box64_RegisterHostThunkDispatcher")
    );

    gBox64Syms.fnRunEntry = reinterpret_cast<FnBox64RunX64Entry>(::dlsym(handle, "Box64_RunX64EntryPoint"));
    if (!gBox64Syms.fnRunEntry) {
        gBox64Syms.fnRunEntry = reinterpret_cast<FnBox64RunX64Entry>(::dlsym(handle, "RunX64EntryPoint"));
    }
    gBox64Syms.fnDynaRun = reinterpret_cast<FnBox64EmuRun>(::dlsym(handle, "DynaRun"));

    if (!gBox64Syms.fnRunEntry && !gBox64Syms.fnDynaRun) {
        LOGW("Đã nạp %s nhưng không tìm thấy điểm vào Box64_RunX64EntryPoint hoặc DynaRun", loadedPath.c_str());
        gBox64Syms.status = Box64RuntimeStatus::SymbolResolutionFailed;
        return gBox64Syms.status;
    }

    if (gBox64Syms.fnRegisterBridge) {
        gBox64Syms.fnRegisterBridge(&cBridgeCallbackAdapter);
    }

    LOGI("Đã nạp thành công bộ dịch mã động ARM64 Box64 từ: %s", loadedPath.c_str());
    gBox64Syms.status = Box64RuntimeStatus::Ready;
    return Box64RuntimeStatus::Ready;
#endif
}

} // namespace

Box64RuntimeStatus Arm64Box64Bridge::probeRuntime(const std::string& dataDirectory) {
    std::lock_guard<std::mutex> lock(gThunkMutex);
    return loadBox64SymbolsLocked(dataDirectory);
}

void Arm64Box64Bridge::defaultTenArgIntegerMarshaller(void* nativeTarget, WinX64CallContext& ctx) {
    if (!nativeTarget) return;

    // Microsoft x64 ABI:
    //   Tham số 1..4 nằm trong RCX, RDX, R8, R9.
    //   [RSP + 0x00] là Return Address, [RSP + 0x08 .. 0x20] là 32-byte Shadow Space,
    //   Tham số 5..10 nằm liên tiếp từ [RSP + 0x28] trở đi.
    uint64_t stackArgs[6] = {0, 0, 0, 0, 0, 0};
    if (ctx.rsp != 0) {
        const auto* stackPtr = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(ctx.rsp + 0x28u));
        for (size_t i = 0; i < 6u; ++i) {
            stackArgs[i] = stackPtr[i];
        }
    }

    // AAPCS64 (ARM64): 8 tham số đầu tự động truyền qua thanh ghi X0..X7, tham số 9..10 trên stack ARM64
    using Aapcs64Fn10 = uint64_t (*)(
        uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t
    );
    auto fn = reinterpret_cast<Aapcs64Fn10>(nativeTarget);
    ctx.rax = fn(
        ctx.rcx, ctx.rdx, ctx.r8, ctx.r9,
        stackArgs[0], stackArgs[1], stackArgs[2], stackArgs[3],
        stackArgs[4], stackArgs[5]
    );
}

void* Arm64Box64Bridge::registerHostFunctionThunk(
    const char* debugName,
    void* nativeTarget,
    HostThunkHandler customMarshaller
) {
    if (nativeTarget == nullptr) return nullptr;

#if defined(__x86_64__) || defined(_M_X64)
    (void)debugName;
    (void)customMarshaller;
    return nativeTarget;
#else
    std::lock_guard<std::mutex> lock(gThunkMutex);
    for (const auto& existing : gThunkTable) {
        if (existing.nativeTarget == nativeTarget && existing.marshaller == (customMarshaller ? customMarshaller : &defaultTenArgIntegerMarshaller)) {
            return existing.x64StubAddr;
        }
    }

    if (!ensureThunkArenaLocked() || gThunkTable.size() >= kMaxHostThunks) {
        return nativeTarget;
    }

    const uint32_t thunkId = static_cast<uint32_t>(gThunkTable.size());
    uint8_t* slotPtr = gThunkCodeArena + static_cast<size_t>(thunkId) * kThunkSlotBytes;
    writeX64HostEscapeStub(slotPtr, thunkId);

    ThunkEntry entry{};
    entry.debugName = debugName;
    entry.nativeTarget = nativeTarget;
    entry.marshaller = customMarshaller ? customMarshaller : &defaultTenArgIntegerMarshaller;
    entry.x64StubAddr = slotPtr;
    gThunkTable.push_back(entry);
    return slotPtr;
#endif
}

bool Arm64Box64Bridge::dispatchHostThunk(uint32_t thunkId, WinX64CallContext& ctx) {
    ThunkEntry entry{};
    {
        std::lock_guard<std::mutex> lock(gThunkMutex);
        if (thunkId >= gThunkTable.size()) {
            return false;
        }
        entry = gThunkTable[thunkId];
    }
    if (!entry.nativeTarget || !entry.marshaller) {
        return false;
    }
    entry.marshaller(entry.nativeTarget, ctx);
    return true;
}

bool Arm64Box64Bridge::executePeEntryPoint(
    void* imageBase,
    uint32_t entryPointRva,
    const std::string& dataDirectory
) {
    if (imageBase == nullptr || entryPointRva == 0) {
        return false;
    }

    Box64RuntimeStatus st = probeRuntime(dataDirectory);
    if (st != Box64RuntimeStatus::Ready) {
        LOGW("ARM64 Box64 Runtime chưa sẵn sàng (mã trạng thái=%d). Hãy đặt libbox64.so vào thư mục dữ liệu hoặc cài bản APK x86_64Only trên thiết bị hỗ trợ ndk_translation.",
             static_cast<int>(st));
        return false;
    }

    if (!Win32Bridge_SetupCurrentThreadTEB()) {
        LOGW("Khởi tạo ngữ cảnh TEB giả lập cho luồng ARM64 Box64 trả về false, tiếp tục với TLS mềm");
    }

    FnBox64Init fnInit = nullptr;
    FnBox64RunX64Entry fnRunEntry = nullptr;
    {
        std::lock_guard<std::mutex> lock(gThunkMutex);
        if (gThunkCodeArena != nullptr) {
            ::mprotect(gThunkCodeArena, kThunkArenaBytes, PROT_READ | PROT_EXEC);
        }
        fnInit = gBox64Syms.fnInit;
        fnRunEntry = gBox64Syms.fnRunEntry;
    }

    if (fnInit != nullptr) {
        const char* argv[] = { "sgre_steam.exe", nullptr };
        fnInit(1, argv, nullptr);
    }

    if (fnRunEntry != nullptr) {
        constexpr size_t kGuestStackBytes = 8u * 1024u * 1024u; // 8 MB Win32 main thread stack
        void* guestStack = ::mmap(
            nullptr, kGuestStackBytes, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
        );
        if (guestStack == MAP_FAILED || guestStack == nullptr) {
            LOGE("Không thể cấp phát ngăn xếp x86_64 8MB cho Box64");
            return false;
        }
        // Căn lề 16-byte theo chuẩn Microsoft x64 ABI (RSP % 16 == 8 ngay tại điểm vào hàm sau lệnh CALL)
        uintptr_t stackTop = (reinterpret_cast<uintptr_t>(guestStack) + kGuestStackBytes - 64u) & ~static_cast<uintptr_t>(0xFu);
        stackTop -= 8u;

        uintptr_t entryVa = reinterpret_cast<uintptr_t>(static_cast<uint8_t*>(imageBase) + entryPointRva);
        LOGI("Chuyển quyền điều khiển sang Box64 DynaRec tại EntryPoint=%p (GuestStackTop=%p)",
             reinterpret_cast<void*>(entryVa), reinterpret_cast<void*>(stackTop));
        fnRunEntry(entryVa, stackTop);
        ::munmap(guestStack, kGuestStackBytes);
        return true;
    }

    LOGE("Box64_RunX64EntryPoint không khả dụng để thực thi PE trực tiếp trong bộ nhớ");
    return false;
}

} // namespace sgre
