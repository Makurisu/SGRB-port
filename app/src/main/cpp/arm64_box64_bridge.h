#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sgre {

enum class Box64RuntimeStatus : int32_t {
    NativeX86_64 = 0,            // Đang chạy trực tiếp trên x86_64 hoặc qua hệ thống libndk_translation
    Ready = 1,                   // Đang chạy trên ARM64 và đã nạp thành công libbox64.so
    MissingRuntimeLibrary = 2,   // Đang chạy trên ARM64 nhưng chưa có libbox64.so trong thiết bị
    SymbolResolutionFailed = 3,  // Tìm thấy libbox64.so nhưng thiếu biểu tượng khởi chạy
    ExecutionFailed = 4          // Lỗi trong quá trình khởi tạo máy ảo hoặc thực thi EntryPoint
};

// Ngữ cảnh thanh ghi chuẩn Microsoft x64 ABI để chuyển tiếp lệnh gọi sang AAPCS64 C++ trên ARM64
struct WinX64CallContext {
    uint64_t rcx = 0;
    uint64_t rdx = 0;
    uint64_t r8 = 0;
    uint64_t r9 = 0;
    uint64_t rsp = 0;       // [rsp + 0x28 + i*8] chứa tham số thứ 5 trở đi (sau 32-byte shadow space)
    uint64_t rax = 0;       // Giá trị trả về nguyên / con trỏ 64-bit
    double xmm[4] = {0.0, 0.0, 0.0, 0.0};
    double xmmRet = 0.0;
};

using HostThunkHandler = void (*)(void* nativeTarget, WinX64CallContext& ctx);

class Arm64Box64Bridge {
public:
    // Kiểm tra trạng thái sẵn sàng của tầng dịch mã x86_64 trên kiến trúc hiện tại
    static Box64RuntimeStatus probeRuntime(const std::string& dataDirectory);

    // Đăng ký một hàm C++ phía host (Win32/D3D11/COM) để tạo địa chỉ stub x86_64 16-byte cho Box64 gọi ngược
    static void* registerHostFunctionThunk(
        const char* debugName,
        void* nativeTarget,
        HostThunkHandler customMarshaller = nullptr
    );

    // Điều phối lời gọi từ stub x86_64 sang hàm C++ ARM64 theo mã định danh thunkId
    static bool dispatchHostThunk(uint32_t thunkId, WinX64CallContext& ctx);

    // Chuyển tiếp mặc định tối đa 10 tham số nguyên/con trỏ từ Microsoft x64 ABI (RCX, RDX, R8, R9 + shadow stack) sang AAPCS64
    static void defaultTenArgIntegerMarshaller(void* nativeTarget, WinX64CallContext& ctx);

    // Khởi tạo Box64 DynaRec và thực thi PE EntryPoint trên ARM64
    static bool executePeEntryPoint(
        void* imageBase,
        uint32_t entryPointRva,
        const std::string& dataDirectory
    );
};

} // namespace sgre
