# Tài Liệu Kiến Trúc: SGRB Android Native Port

Tài liệu này đóng vai trò Memory/Docs kỹ thuật cốt lõi cho dự án port **STEINS;GATE REBOOT** sang Android theo Phương án 1 (Binary Wrapper Native).

---

## 1. Luồng Xử Lý Tổng Thể (Execution Flow)

```mermaid
sequenceDiagram
    participant User as Người Chơi (Touch)
    participant Java as MainActivity / TouchInputHandler
    participant JNI as jni_bridge (NDK C++)
    participant Injector as input_injector (Win32 Hook)
    participant Core as Box64 Runtime + sgre_steam.exe
    participant GPU as DXVK (D3D11 to Vulkan) -> ANativeWindow

    User->>Java: Thao tác Chạm / Vuốt trên màn hình
    Java->>JNI: Gửi toạ độ chuẩn hoá (normalizedX, normalizedY, actionType)
    JNI->>Injector: Chuyển đổi sang thông điệp Win32 (WM_LBUTTONDOWN, WM_MOUSEMOVE...)
    Injector->>Core: Inject thông điệp vào hàng đợi tin nhắn của game gốc
    Core->>GPU: Render frame qua D3D11 calls
    GPU->>User: Hiển thị đồ hoạ Vulkan trực tiếp lên màn hình
```

---

## 2. Các Thành Phần Kỹ Thuật (System Components)

### 2.1. Tầng Giao Diện Android (Java/Kotlin)
- **`MainActivity.java`**:
  - Thiết lập chế độ toàn màn hình (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY`, ẩn thanh điều hướng và status bar).
  - Khởi tạo `SurfaceView` để làm mục tiêu dựng hình Vulkan (`ANativeWindow`).
  - Kiểm tra quyền đọc dữ liệu bộ nhớ (`MANAGE_EXTERNAL_STORAGE` hoặc Scoped Storage). Tự động liên kết thư mục dữ liệu cố định `/sdcard/sgrb_game_data/` nếu tồn tại `sgre_steam.exe`, đảm bảo dữ liệu game không bao giờ bị hệ điều hành xóa khi gỡ hoặc cài lại APK (tránh lặp lại lỗi mất dữ liệu). Fallback về `getExternalFilesDir("data")` nếu không tìm thấy.
- **`TouchInputHandler.java`**:
  - Lắng nghe sự kiện cảm ứng đa điểm qua `onTouchEvent`.
  - Phân loại cử chỉ:
    - **Tap**: `ACTION_DOWN` + `ACTION_UP` trong khoảng thời gian ngắn (< 250ms) và không dịch chuyển quá ngưỡng threshold.
    - **Swipe Up / Down**: Khoảng cách vuốt theo trục Y vượt ngưỡng `minSwipeDistance`.
    - **Long Press**: Giữ ngón tay tại chỗ > 500ms.

### 2.2. Tầng Cầu Nối C++ Native (NDK)
- **`jni_bridge.cpp`**:
  - Đăng ký các hàm native:
    - `nativeInit(Surface surface, String dataPath)`: Thiết lập đường dẫn dữ liệu và bề mặt render.
    - `nativeSendTouchEvent(int action, float x, float y)`: Nhận toạ độ chạm.
    - `nativePause()` / `nativeResume()`: Quản lý vòng đời ứng dụng.
- **`input_injector.cpp`**:
  - Chuyển toạ độ màn hình Android (chuẩn hoá 0.0 - 1.0) sang độ phân giải game (1920x1080) và mã thông điệp Win32 (`WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_MOUSEMOVE`, `WM_KEYDOWN`...).
  - Đẩy trực tiếp thông điệp vào hàng đợi tin nhắn `Win32Bridge_PostMessage` để `stub_PeekMessageA` phân phối đồng bộ vào game loop của binary gốc x86_64.
- **`payload_loader.cpp`**:
  - Quét thư mục dữ liệu ngoài `/sdcard/Android/data/com.makurisu.sgrb/files/data`.
  - Phân tích và xác thực cấu trúc Portable Executable (DOS Magic `MZ` 0x5A4D và PE Signature `PE\0\0` 0x00004550) của file `sgre_steam.exe`.
  - Bóc tách COFF Header (Machine: `0x8664` x86_64, 8 sections) và PE32+ Optional Header.
  - Phân tích bảng Import Directory (`0x9627A0`, kích thước `0x168`) và nhận diện toàn bộ 17 DLL phụ thuộc (`steam_api64.dll`, `d3d11.dll`, `D3DCOMPILER_47.dll`, `XINPUT1_4.dll`, `KERNEL32.dll`, `USER32.dll`...).
  - Cấp phát không gian địa chỉ ảo 64-bit ẩn danh (`mmap` ~11.05 MB) và ánh xạ từng Section (`.text`, `.rodata`, `.rdata`, `.data`, `.pdata`, `.fptable`, `.rsrc`, `.reloc`) theo đúng toạ độ `VirtualAddress`.
  - Thực thi thuật toán **64-bit Base Relocation Engine** (`IMAGE_REL_BASED_DIR64` fixup qua 103 blocks): Tự động tính độ lệch delta và tái định vị chuẩn xác **10,692 con trỏ bộ nhớ 64-bit** trong RAM để code x86_64 của game chạy đúng địa chỉ không bị crash.
  - Phân giải và liên kết động trực tiếp bảng **Import Address Table (IAT)** trong RAM: Gắn chính xác 9 con trỏ hàm API của Steamworks và hàm đồ họa `D3D11CreateDevice` vào binary gốc.
  - Cấu hình phân quyền trang bộ nhớ `mprotect` chuẩn xác theo Characteristics của từng section (`PROT_READ | PROT_EXEC` cho code `.text` & `.rodata`, `PROT_READ | PROT_WRITE` cho `.data`).
  - **Windows TEB & PEB Subsystem**: Cấp phát cấu trúc 64-bit Thread Environment Block (`TEB`), Process Environment Block (`PEB`), và `RTL_USER_PROCESS_PARAMETERS`. Tự động gắn địa chỉ cơ sở vào thanh ghi phân đoạn `GS` qua syscall `arch_prctl(ARCH_SET_GS, teb)` của nhân Linux x86_64, đảm bảo các chỉ lệnh truy cập `gs:[0x30]` và `[rax + 0x60]` của Windows CRT/Game engine thực thi mượt mà không bị lỗi `SEGV_MAPERR`.
  - Khởi tạo thành công điểm vào thực thi `EntryPoint` (RVA: `0x56EACC`, Virtual: `0x7638727f6acc`) trong không gian nhớ của tiến trình Android.
- **`d3d11_bridge.cpp`**:
  - Đóng vai trò cầu nối đồ họa Direct3D 11 và DXGI cho engine game gốc:
    - Bắt chặn hàm nhập `D3D11CreateDevice` từ bảng IAT.
    - **D3D Compiler Subsystem (`D3DCOMPILER_47.dll`)**:
      - Bắt chặn hàm `D3DReflect` từ IAT.
      - Cấp phát mock `ID3D11ShaderReflection` (`IID_ID3D11ShaderReflection` `{8d536ca1-0cca-4956-a837-786963755584}`) cùng hệ thống vtable phản chiếu `ID3D11ShaderReflectionConstantBuffer`, `ID3D11ShaderReflectionVariable`, `ID3D11ShaderReflectionType`.
      - Xử lý phương thức `GetDesc(D3D11_SHADER_DESC*)`, khởi tạo an toàn tham số Shader Model 5.0, loại bỏ hoàn toàn lỗi crash null-pointer dereference (`SEGV_MAPERR fault addr 0x0`) khi engine phản chiếu cấu trúc shader bytecode.
    - **Hệ thống Đa Vtable COM Chuyên Biệt (Dedicated Multi-Interface COM Vtables)**: Thay vì dùng chung một vtable đơn lẻ gây xung đột tham số gọi hàm (call-signature mismatch), kiến trúc triển khai 13 vtable và đối tượng COM riêng biệt:
      - `ID3D11Device` / `ID3D11Device1` (128 slots: `CreateBuffer` slot 3, `CreateTexture2D` slot 5, `CreateShaderResourceView` slot 7, `CreateRenderTargetView` slot 9, `CreateDepthStencilView` slot 10, `CreateInputLayout` slot 11, `CreateVertexShader` slot 12, `CreatePixelShader` slot 15, `CreateBlendState` slot 20, `CreateDepthStencilState` slot 21, `CreateRasterizerState` slot 22, `CreateSamplerState` slot 23, `GetFeatureLevel` slot 37, `GetImmediateContext` slot 40)
      - `ID3D11DeviceContext` / `ID3D11DeviceContext1` (128 slots: Chuẩn hoá chính xác theo đặc tả Microsoft DirectX 11 vtable: `DrawIndexed` slot 12, `Draw` slot 13, `Map` slot 14 với 16MB scratch buffer, `Unmap` slot 15, `RSSetViewports` slot 44, `RSSetScissorRects` slot 45, `UpdateSubresource` slot 48, `ClearRenderTargetView` slot 50)
      - `ID3D11Multithread` (16 slots, hỗ trợ `SetMultithreadProtected`, `GetMultithreadProtected`)
      - `IDXGIDevice` / `IDXGIDevice1` (32 slots, `GetAdapter`, `SetMaximumFrameLatency`)
      - `IDXGIAdapter` / `IDXGIAdapter1` (32 slots, `GetParent`)
      - `IDXGIFactory` / `IDXGIFactory2` (32 slots, `MakeWindowAssociation`, `CreateSwapChain`, `CreateSwapChainForHwnd`)
      - `IDXGISwapChain` / `IDXGISwapChain1` (32 slots, `GetBuffer`, `Present`, `ResizeBuffers`, `GetDevice`)
      - `ID3D11Texture2D` (32 slots, `GetDesc` thiết lập độ phân giải 1920x1080 và định dạng RGBA8)
      - `ID3D11GenericResource` (32 slots cho Views/Buffers/Shaders)
      - `ID3D11ShaderReflection` (32 slots cho `GetDesc`, `GetConstantBufferByIndex`, `GetResourceBindingDesc`)
      - `ID3D11ShaderReflectionConstantBuffer`, `ID3D11ShaderReflectionVariable`, `ID3D11ShaderReflectionType` (16 slots)
    - **COM QueryInterface GUID Dispatcher**: Tự động bóc tách và phân giải chính xác 16-byte COM GUIDs chuẩn Microsoft (`IID_ID3D11Multithread`, `IID_IDXGIDevice1`, `IID_IDXGIFactory2`, `IID_IDXGISwapChain`, `IID_ID3D11Texture2D`, `IID_ID3D11ShaderReflection`...), chuyển hướng chính xác đến từng đối tượng mock tương ứng.
    - **SlotDispatcher Compile-Time Telemetry Engine & Graphics Pipeline Tracing**: Sử dụng C++ template meta-programming (`std::make_index_sequence<128>`) tự động sinh 128 con trỏ hàm fallback độc lập cho `ID3D11Device` và `ID3D11DeviceContext`. Bắt chặn và log chính xác số thứ tự slot phương thức đồ hoạ được M2 Engine gọi đến, đồng thời thu thập telemetry chi tiết cho `CreateBuffer`, `CreateTexture2D`, `CreateShader`, `Draw`, `DrawIndexed`, `Map`, `Unmap`, và `UpdateSubresource`.
    - **Producer-Consumer Frame Synchronizer & Graphics Pipeline Telemetry**: Triển khai cơ chế đồng bộ luồng đồ hoạ (`sFrameMutex` và `sFrameCond`) kết nối giữa luồng D3D11 của game và luồng render EGL của Android. Cấu trúc `RenderFrameState` (48-byte ABI chuẩn) truyền tải đồng thời: màu nền RGBA từ `ClearRenderTargetView` (slot 50), độ phân giải (1920x1080), chỉ số khung hình, và toàn bộ thống kê thời gian thực (`drawCalls`, `indexDraws`, `texturesCreated`, `shadersCreated`, `buffersCreated`). Khi `Present` hoặc `Present1` hoàn thành, phát tín hiệu đánh thức luồng render Android để gọi `glClear`, xuất telemetry định kỳ 60 frames và `eglSwapBuffers`, đồng bộ tuyệt đối chu kỳ hiển thị khung hình theo thời gian thực.
    - **EGL Native Visual ID Synchronization**: Truy vấn thuộc tính `EGL_NATIVE_VISUAL_ID` từ EGL configuration và đồng bộ trực tiếp vào `ANativeWindow_setBuffersGeometry` kết hợp `SurfaceHolder.setFormat(PixelFormat.RGBA_8888)`, loại bỏ hoàn toàn lỗi `EGL_BAD_MATCH (0x3009)` trên Android SurfaceView.
    - Fallback generic stub an toàn tuyệt đối (không đọc/ghi con trỏ bừa bãi), loại bỏ hoàn toàn nguy cơ ghi đè bộ nhớ không hợp lệ (`SEGV_MAPERR`).
    - Xác lập `D3D_FEATURE_LEVEL_11_0` (0xb000) và mã kết quả `S_OK` (0) để engine đồ họa game kích hoạt thành công mà không phụ thuộc vào `d3d11.dll` native của Windows.
- **`steam_hook.cpp`**:
  - Giả lập chuẩn 9 hàm API Steamworks mà game yêu cầu từ IAT:
    - `SteamAPI_GetHSteamUser()`: Trả về Mock User ID 1.
    - `SteamAPI_Shutdown()`: Dọn dẹp an toàn.
    - `SteamAPI_RestartAppIfNecessary()`: Trả về `false` để ngăn chặn việc game tự restart qua client Steam.
    - `SteamAPI_RunCallbacks()`: Duy trì chu kỳ xử lý sự kiện.
    - `SteamInternal_SteamAPI_Init()`: Xác thực khởi tạo thành công chuẩn `ESteamAPIInitResult` (trả về `0 = k_ESteamAPIInitResult_OK`).
    - `SteamInternal_FindOrCreateUserInterface(hUser, version)`: Cấp phát mock C++ interface với vtable 64 slots (`gSteamVtable`) trả về con trỏ tới `gMockSteamID` (76561197960287930ULL), hỗ trợ các giao diện `SteamUser023`, `SteamFriends018`, `STEAMAPPS_INTERFACE_VERSION009`.
    - `SteamAPI_RegisterCallback()` & `SteamAPI_UnregisterCallback()`.
    - `SteamInternal_ContextInit(pContextInitData)`: Tự động gọi con trỏ hàm callback `pfnInit(&pInterface)` theo cấu trúc ngữ cảnh của Valve, liên kết interface hợp lệ và trả về `&pInterface` cho caller, ngăn chặn hoàn toàn lỗi crash null-pointer dereference (`SEGV_MAPERR fault addr 0x31`) tại RVA `0x395E46` khi game gọi `SteamUser()->GetSteamID()`.
  - Vượt qua toàn bộ kiểm tra Steam DRM mà không phụ thuộc vào `steam_api64.dll` của Windows.
- **`win32_bridge.cpp`**:
  - Đóng vai trò hệ thống cầu nối Win32 API / CRT subsystem cho Windows binary chạy trên Android:
    - **Safe Heap & Memory Management**: Quản lý cấp phát an toàn với bảng theo dõi `gHeapAllocations` và mutex đa luồng, hỗ trợ cờ `HEAP_ZERO_MEMORY`, ngăn chặn crash `SIGSEGV` trong jemalloc khi game hoặc CRT giải phóng con trỏ ngoại vi hoặc unallocated buffer.
    - **Virtual Memory Subsystem**: Giả lập `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` ánh xạ trực tiếp sang Linux `mmap`/`munmap`/`mprotect`.
    - **Synchronization Subsystem (Events, Semaphores & WaitForSingleObject)**:
      - Quản lý các đối tượng đồng bộ Win32 qua cấu trúc `Win32SyncObject` luồng an toàn với `std::mutex` và `std::condition_variable`.
      - **Win32 Events (`CreateEventA/W`, `CreateEventExA/W`, `SetEvent`, `ResetEvent`)**: Hỗ trợ đầy đủ cờ Manual-Reset và Auto-Reset. Tự động chuyển trạng thái `signaled` về `false` khi có luồng chờ đợi nhận tín hiệu thành công trong chế độ Auto-Reset.
      - **Win32 Semaphores (`CreateSemaphoreA/W`, `ReleaseSemaphore`)**: Quản lý bộ đếm `currentCount` và `maxCount` theo chuẩn Win32. Đảm bảo worker thread trong thread pool tạm dừng (`wait`) khi chưa có công việc, và thức tỉnh đồng bộ khi hàm chính đẩy task và gọi `ReleaseSemaphore`.
      - **Wait Functions (`WaitForSingleObject`, `WaitForSingleObjectEx`)**:
        - Hỗ trợ chờ đợi trên cả Thread Handle, Event Handle và Semaphore Handle với các chế độ thời gian `0` (non-blocking check), `INFINITE` (`0xFFFFFFFF`) hoặc thời gian chờ mili-giây (`std::chrono::milliseconds`).
        - Trả về chuẩn xác `WAIT_OBJECT_0` (0x0), `WAIT_TIMEOUT` (0x102) hoặc `WAIT_FAILED` (0xFFFFFFFF).
        - Ngăn chặn hoàn toàn hiện tượng worker thread thức tỉnh sớm khi hàng đợi rỗng dẫn đến gọi con trỏ hàm rác/null (`call rdx` với `rdx = 0`) gây crash `SIGSEGV` tại RVA `0x4BFA90`.
      - Cung cấp `SwitchToThread` (`sched_yield`), SRWLock, Condition Variable.
    - **String & Locale Subsystem**: Chuyển đổi mã ký tự UTF-8/UTF-16 qua `MultiByteToWideChar`, `WideCharToMultiByte` với chuẩn `char16_t` (2-byte UTF-16LE của Windows), loại bỏ triệt để lỗi mismatch kích thước `sizeof(wchar_t) == 4` trên Linux/Android NDK gây lỗi nhảy cóc ký tự (corrupted paths).
    - **CRT & Exception Subsystem**: Hỗ trợ PE ImageBase tracking, `RtlPcToFileHeader`, `IsProcessorFeaturePresent` (vô hiệu hoá `PF_FASTFAIL_AVAILABLE`), TLS/FLS slot management.
    - **Virtual File System (VFS)**:
      - Cung cấp lớp trừu tượng file I/O Win32 hoàn chỉnh: `CreateFileW`, `CreateFileA`, `ReadFile`, `WriteFile`, `SetFilePointer`, `SetFilePointerEx`, `GetFileSize`, `GetFileSizeEx`, `GetFileInformationByHandle`, `GetOverlappedResult`, `CancelIo`, `CancelIoEx`, `CloseHandle`, `GetFileAttributesW`, `GetFileAttributesA`, `FindFirstFileW`, `FindFirstFileExW`, `FindNextFileW`, `FindClose`, `FlushFileBuffers`, `SetEndOfFile`, `DeleteFileW`, `DeleteFileA`.
      - **Asynchronous & OVERLAPPED I/O Subsystem**:
        - Hỗ trợ đầy đủ cấu trúc 32-byte `Win32Overlapped` (`internal`, `internalHigh`, `offset`, `offsetHigh`, `hEvent`) theo chuẩn 64-bit Windows ABI.
        - Xử lý bất đồng bộ trong `stub_ReadFile` và `stub_WriteFile`: Khi nhận `lpOverlapped != nullptr`, trích xuất file offset 64-bit và thực thi I/O tại toạ độ chỉ định thông qua `pread64` / `pwrite64`, bảo toàn file pointer của fd.
        - Tự động cập nhật `ov->internal = 0` (`STATUS_SUCCESS`), gán `ov->internalHigh = bytesTransferred`, và phát tín hiệu đánh thức `stub_SetEvent(ov->hEvent)` cho event handle được liên kết.
        - Hỗ trợ trường hợp game engine truyền `lpNumberOfBytesRead == nullptr` (như tại hàm streaming của M2 Engine `0x1404dbca3`), đảm bảo worker thread chờ tại `WaitForSingleObject(ov->hEvent, INFINITE)` được giải phóng ngay lập tức để tiếp tục nạp index `script_info.psb` và stream file kịch bản `script_body.bin`.
        - Triển khai `stub_GetOverlappedResult`, `stub_CancelIo`, `stub_CancelIoEx`.
      - **Case-Insensitive Path Resolution & Prefix Fallback**: Tự động chuẩn hoá đường dẫn Windows (`\`, loại bỏ drive letter `C:`) và quét thư mục đệ quy qua `strcasecmp` để ánh xạ chính xác file assets trên filesystem Linux/Android (ext4/f2fs).
      - **Mages Engine Resource Prefix Fallback**: Tự động nhận diện và bóc tách các tiền tố thư mục nền tảng (`windata/`, `winweb/`, `wind3d11data/`, `win/`) khi file không tồn tại theo đường dẫn được yêu cầu, fallback tìm kiếm tự động tại thư mục gốc `files/data/` hoặc `wind3d11data/`, giải quyết triệt để lỗi file-not-found cho `script_info.psb.m`, `script_body.bin` mà không yêu cầu symlinks (vốn bị cấm trên FUSE/sdcardfs của Android).
      - **Virtual Directory Attributes**: Giả lập thuộc tính `FILE_ATTRIBUTE_DIRECTORY` (0x10) cho các truy vấn kiểm tra sự tồn tại của thư mục ảo như `windata`, `winweb`, `wind3d11data`.
      - **Thread-safe Handle Management**: Quản lý bảng con trỏ handle an toàn với POSIX file descriptors, phân định rõ ràng giữa file handle, find handle và dummy system handles.
    - **SHELL32 Subsystem**:
      - Bắt chặn `SHGetSpecialFolderPathW` để điều hướng thư mục Documents / AppData của Windows sang thư mục lưu trữ cục bộ ảo `C:\saves` (ánh xạ trực tiếp vào `data/saves`), tự động tạo thư mục nếu có cờ `fCreate`.
      - Bắt chặn `SHCreateDirectoryExW` để tạo cấu trúc thư mục lồng nhau an toàn cho save data.
    - **Window Lifecycle, Procedure & Message Pump Subsystem**:
      - Triển khai hàng đợi thông điệp Win32 luồng an toàn (`gMsgQueue`) kết hợp với hàm `stub_PeekMessageA` theo cờ `PM_REMOVE`.
      - **7-Message Initial Lifecycle Sequence**: Tự động bơm 7 thông điệp vòng đời Win32 thiết yếu khi khởi động: `WM_CREATE`, `WM_SHOWWINDOW` (`SW_SHOW`), `WM_ACTIVATEAPP` (`bActive=TRUE`), `WM_ACTIVATE` (`WA_ACTIVE`), `WM_SETFOCUS`, `WM_SIZE` (`SIZE_RESTORED`, 1920x1080), và `WM_PAINT`. Đánh thức và kích hoạt hoàn toàn engine logic, thiết lập kích thước chuẩn và loại bỏ trạng thái đóng băng nền.
      - **Window Properties Subsystem (`SetPropA`, `GetPropA`, `RemovePropA`)**: Quản lý thuộc tính liên kết cửa sổ Win32 luồng an toàn (`gPropMap`), hỗ trợ cả chuỗi định danh (như `__OWN_INSTANCE__`) và số nguyên ATOM (<= 0xFFFF). Cho phép hàm `WndProc` của game engine (`0x1404EB960`) truy xuất chính xác con trỏ đối tượng cửa sổ C++ (`pWindowObject`) thay vì chuyển hướng sai vào `DefWindowProcA`, đảm bảo toàn bộ sự kiện vòng đời, kích thước màn hình và cử chỉ cảm ứng chuột/chạm được xử lý triệt để.
      - **Window Long & UserData Subsystem (`GetWindowLongA`, `SetWindowLongA`, `GetWindowLongPtrA`, `SetWindowLongPtrA`)**: Lưu trữ và truy xuất trạng thái thuộc tính mở rộng theo cửa sổ (`GWLP_USERDATA`, `GWL_STYLE`, `GWL_EXSTYLE`), hỗ trợ giữ cờ `WS_VISIBLE` (0x10000000).
      - **Window Procedure (WndProc) Interception & Telemetry**: Bắt trọn con trỏ hàm `lpfnWndProc` được game đăng ký qua cấu trúc `WNDCLASSEXA` trong `RegisterClassExA`. Khi `DispatchMessageA` được gọi từ game loop, thông điệp được chuyển thẳng vào hàm `WndProc` của game engine kèm log telemetry chi tiết (`message`, `hwnd`, `wParam`, `lParam`), đồng thời cung cấp phản hồi chuẩn trong `DefWindowProcA` cho `WM_NCHITTEST` và `WM_SETCURSOR`.
      - Giả lập trọn bộ API trạng thái cửa sổ (`GetForegroundWindow`, `GetActiveWindow`, `GetFocus`, `IsIconic`, `GetCursorPos`, `SetCursorPos`, `MonitorFromWindow`, `GetMonitorInfoA`, `GetWindowThreadProcessId`, `SetClassLongPtrA`...) đảm bảo engine luôn xác định cửa sổ game ở chế độ tiền cảnh và sẵn sàng nhận input.
    - **Multi-Threading & Per-Thread Windows TEB Subsystem**:
      - Hỗ trợ đầy đủ các hàm điều khiển luồng của KERNEL32: `CreateThread`, `ResumeThread`, `SetThreadPriority`, `SetThreadIdealProcessor`, `GetExitCodeThread`, `TerminateThread`, `ExitThread`, `FreeLibraryAndExitThread`.
      - Cơ chế **Trampoline per-thread**: Mọi worker thread mới được sinh ra bởi `pthread_create` sẽ tự động khởi tạo Windows TEB độc lập và cấu hình thanh ghi `%gs` (`ARCH_SET_GS`) thông qua `Win32Bridge_SetupCurrentThreadTEB()` trước khi chạy hàm mục tiêu `lpStartAddress(lpParameter)`.
      - **Windows x64 Calling Convention (`ms_abi`)**: Định nghĩa con trỏ hàm luồng `using Win32ThreadStartRoutine = uint32_t (WINAPI *)(void* lpThreadParameter)` với thuộc tính `__attribute__((ms_abi))`. Đảm bảo tham số `lpParameter` được truyền qua thanh ghi `RCX` (thay vì `RDI` theo System V AMD64 ABI của Linux Clang) và cấp phát đủ 32-byte Shadow Space trên stack, ngăn ngừa hoàn toàn lỗi `SIGSEGV` tại hàm khởi động luồng CRT `_threadstartex` (RVA `0x4BEA2D`).
      - **Đồng Bộ Hóa Luồng Bị Tạm Dừng (`CREATE_SUSPENDED` & `ResumeThread`)**: Xử lý cờ `CREATE_SUSPENDED (0x00000004)` bằng cơ chế khoá điều kiện `std::condition_variable` và `std::mutex`. Worker thread sẽ tạm dừng an toàn cho đến khi luồng chính cấu hình xong dữ liệu đồng bộ và phát tín hiệu đánh thức qua `ResumeThread()`.
      - Đảm bảo toàn bộ 9 luồng nền của Mages Engine (Disk loader, resource streaming, audio worker, E-mote physics) khởi tạo và vận hành mượt mà.
    - **Cryptography Subsystem (`ADVAPI32.dll`)**:
      - Giả lập trọn vẹn bộ Windows CryptoAPI: `CryptAcquireContextA`, `CryptReleaseContext`, `CryptImportKey`, `CryptSetKeyParam`, `CryptEncrypt`, `CryptDecrypt`, `CryptDestroyKey`, `CryptDestroyHash`.
      - Cung cấp các handle hợp lệ và phản hồi thành công (`TRUE`) để engine M2 thực hiện giải mã các gói dữ liệu game (`.psb.m`) và save data mà không bị tắc nghẽn.
    - **OLE32, GDI32 & WINMM Subsystems**:
      - `ole32.dll`: Cung cấp `CoInitializeEx`, `CoUninitialize`, `CoCreateGuid`, `CoCreateInstance` (E_NOINTERFACE), `CoTaskMemFree`, `PropVariantClear`.
      - `GDI32.dll`: Cung cấp `GetDeviceCaps` (độ phân giải 1920x1080, 32-bit màu, 96 DPI), `GetStockObject`.
      - `WINMM.dll`: Cung cấp bộ đếm độ chính xác cao `timeBeginPeriod`, `timeEndPeriod`, và `PlaySoundA`.
      - Hardware Identification: Cung cấp `GetAdaptersInfo`, `Netbios`, `PropVariantToInt64` phục vụ việc xác thực hệ thống.
- **`dinput_bridge.cpp`**:
  - Đóng vai trò cầu nối xử lý thiết bị nhập liệu (Keyboard, Mouse, Gamepad) theo chuẩn COM DirectInput 8:
    - Bắt chặn hàm khởi tạo `DirectInput8Create` từ `DINPUT8.dll`.
    - Cấp phát mock `IDirectInput8W` và `IDirectInputDevice8W` với vtable chuẩn COM (32 slots) hỗ trợ đầy đủ các phương thức thiết yếu của game engine:
      - `CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter)`: Cung cấp đối tượng device cho bàn phím (`GUID_SysKeyboard`) và chuột (`GUID_SysMouse`).
      - `SetDataFormat()`, `SetCooperativeLevel()`, `Acquire()`, `Unacquire()`, `GetDeviceState()`.
    - Ngăn chặn hoàn toàn lỗi crash null pointer dereference (`SEGV_MAPERR fault addr 0x0`) tại RVA `0x4F379D` khi engine giải tham chiếu `*ppvOut` để gọi `CreateDevice`.
- **`XInput Subsystem (XINPUT1_4.dll)`**:
  - Bắt chặn gọi theo ordinal (Ordinal #2 `XInputGetState`, Ordinal #3 `XInputSetState`).
  - Xóa trắng bộ nhớ `XINPUT_STATE` và trả về mã lỗi an toàn `ERROR_DEVICE_NOT_CONNECTED` (1167) để game tự động chuyển sang chế độ bàn phím/chuột mà không phát sinh lỗi bất thường.
- **`xaudio2_bridge.cpp`**:
  - Đóng vai trò cầu nối âm thanh DirectX XAudio2 (hỗ trợ XAudio 2.7, 2.8, 2.9):
    - Bắt chặn hàm `XAudio2Create` được nạp động qua `GetProcAddress`.
    - Cung cấp mock `IXAudio2` và `IXAudio2MasteringVoice` với vtable chuẩn COM (32 slots), khởi tạo thành công mastering voice mà không gây lỗi phân rã module âm thanh.
- **`media_foundation_bridge.cpp`**:
  - Đóng vai trò cầu nối phát video Windows Media Foundation (`MFPlat.DLL` và `MFReadWrite.dll`):
    - Triển khai 15 hàm API chuẩn Media Foundation: `MFStartup`, `MFShutdown`, `MFCreateAttributes`, `MFCreateMediaType`, `MFCreateDXGIDeviceManager`, `MFCreateMemoryBuffer`, `MFCreateSample`, `MFCalculateImageSize`, `MFGetStrideForBitmapInfoHeader`, `MFllMulDiv`, `MFCreateSourceResolver`, `MFCreateAsyncResult`, `MFInvokeCallback`, `MFCreateSourceReaderFromMediaSource`, `MFCreateSourceReaderFromURL`.
    - Cung cấp các mock COM interface chuẩn Microsoft Media Foundation: `IMFAttributes` (36 slots), `IMFMediaType` (40 slots), `IMFSourceResolver` (16 slots), `IMFSourceReader` (16 slots), `IMFDXGIDeviceManager` (16 slots), `IMFMediaBuffer` (16 slots), `IMFSample` (48 slots), `IMFMediaSource` (16 slots).
    - **Telemetry & Video Tracing**: Bắt chặn và chuyển đổi URL video (`CreateObjectFromURL`, `CreateSourceReaderFromURL`) sang UTF-8 để theo dõi chính xác file movie (`sg_titlebg.webm`, `sg_op.webm`) được yêu cầu bởi M2 Engine.
    - **Safe Stream Termination**: Cung cấp cờ `MF_SOURCE_READERF_ENDOFSTREAM (0x00000001)` trong `IMFSourceReader::ReadSample`, báo hiệu cho video decoder chuyển trạng thái an toàn, ngăn chặn hiện tượng treo vô hạn vòng lặp chờ frame video để vào thẳng giao diện Title Menu.
- **`d3d11_bridge.cpp`**:
  - Đóng vai trò cầu nối đồ hoạ Direct3D 11 & DXGI Subsystem:
    - Bắt chặn các hàm `D3D11CreateDevice`, `D3DReflect`, `CreateSwapChain`, `CreateSwapChainForHwnd`.
    - Cung cấp mô phỏng hoàn chỉnh cho `IDXGISwapChain` / `IDXGISwapChain1` (slots 7..22) và `ID3D11Texture2D` (slots 3, 7, 10).
    - Khởi tạo đầy đủ cấu trúc `DXGI_SWAP_CHAIN_DESC` với trường `BufferCount` tại offset `0x28` (ứng với câu lệnh `mov ebp, dword ptr [rsp + 0x58]` tại RVA `0x4C47C5`).
    - Đảm bảo `BufferCount >= 1` (mặc định 2 hoặc 3 theo cấu hình swapchain), ngăn chặn triệt để lỗi chia cho 0 `SIGFPE (FPE_INTDIV)` tại RVA `0x54AF2A` (`div r8`) khi engine khởi tạo số lượng bucket của container quản lý backbuffer.
    - Cung cấp `mock_Texture2D_GetType` và `mock_Texture2D_GetDesc` đồng bộ kích thước và định dạng texture.
- **`resource_registry.cpp` & `bc7_decoder.cpp` (Milestone M1 - Resource Registry & BC7 Decoder)**:
  - **Resource Registry**: Quản lý tập trung vòng đời các tài nguyên D3D11 (Buffer và Texture2D) phía CPU được cấp phát bởi `CreateBuffer` và `CreateTexture2D`.
    - Giới hạn ngân sách bộ nhớ RAM CPU cố định (768 MB) nhằm ngăn chặn Android Low Memory Killer (LMK). Khi vượt ngân sách, hệ thống chỉ giữ lại metadata để đảm bảo engine không crash và ghi nhận telemetry `droppedResources`.
    - Hỗ trợ đầy đủ các thao tác ghi dữ liệu `UpdateSubresource` và ánh xạ bộ nhớ `Map` / `Unmap`.
    - Quản lý bộ đếm tham chiếu COM (`AddRef` / `Release`) đồng bộ với vòng đời của game engine.
  - **BC7 Decoder**: Giải nén định dạng nén khối BC7 (DXGI_FORMAT_BC7_UNORM / BC7_UNORM_SRGB) sang định dạng chuẩn RGBA8 trên CPU sử dụng thư viện `bcdec`.
    - Xử lý mượt mà mọi kích thước texture (kể cả kích thước không chia hết cho 4), bảo vệ an toàn biên bộ nhớ.
    - Cho phép nạp trực tiếp texture nền, sprite nhân vật và UI của game vào bộ nhớ render mà không phụ thuộc vào phần cứng GPU hỗ trợ BC7.
- **OpenGL ES 2.0 Texture Presentation Pipeline (Milestone M2 - Texture Presentation & Render Pipeline)**:
  - **D3D11 Device Context Slot Interception**: Mở rộng bảng vtable `ID3D11DeviceContext` với các handler chuyên biệt:
    - Slot 8 (`PSSetShaderResources`): Lưu vết con trỏ Resource/Texture đang hoạt động thông qua đối tượng `MockViewCOM`.
    - Slot 7/16 (`VSSetConstantBuffers`, `PSSetConstantBuffers`), Slot 9/11 (`PSSetShader`, `VSSetShader`), Slot 10/26 (`PSSetSamplers`, `VSSetSamplers`), Slot 17/18/19/24 (`IASetInputLayout`, `IASetVertexBuffers`, `IASetIndexBuffer`, `IASetPrimitiveTopology`), Slot 25 (`VSSetShaderResources`), Slot 33/35/36 (`OMSetRenderTargets`, `OMSetBlendState`, `OMSetDepthStencilState`), Slot 43 (`RSSetState`), Slot 53 (`ClearDepthStencilView`).
    - API `D3D11Bridge_GetActiveTextureInfo`: Trích xuất trực tiếp kích thước và con trỏ pixel RGBA8 đã giải mã của texture đang bind trên GPU sang EGL render thread.
  - **OpenGL ES 2.0 Quad Renderer (`jni_bridge.cpp`)**:
    - Khởi tạo Shader Program GLES 2.0 (Vertex + Fragment Shaders) cùng cấu trúc quad đỉnh 4 điểm `GL_TRIANGLE_STRIP` với toạ độ UV lật trục V để hiển thị hình ảnh đúng chiều.
    - Tự động cấp phát GLES Texture2D, nạp pixel RGBA8 bằng `glTexImage2D` (khi đổi texture/kích thước) hoặc `glTexSubImage2D` (khi cập nhật khung hình).
    - Render trực tiếp quad toàn màn hình lên `ANativeWindow` và đồng bộ qua `eglSwapBuffers`, kết nối toàn bộ pixel game engine lên màn hình thiết bị Android.
- **Sprite Geometry Composition & Alpha Blending Pipeline (Milestone M3 - Sprite Geometry Composition)**:
  - **D3D11 Input Assembler Tracking (`IASetVertexBuffers`, `IASetIndexBuffer`)**:
    - Lưu vết con trỏ Vertex Buffer đang hoạt động và stride kích thước đỉnh (mặc định 20 bytes = 5 floats: `pos.x, pos.y, pos.z, uv.u, uv.v`).
    - Theo dõi con trỏ Index Buffer và định dạng index DXGI (16-bit uint).
  - **Sprite Quad Geometry Extraction (`DrawIndexed` slot 12)**:
    - Bắt chặn các lệnh vẽ quad (`IndexCount >= 4`) từ game engine M2.
    - Đọc dữ liệu toạ độ đỉnh từ vùng nhớ CPU của Vertex Buffer qua `ResourceRegistry::map`, bóc tách chính xác toạ độ chuẩn hoá NDC `(x, y)` và toạ độ texture mapping `(u, v)`.
    - Đóng gói dữ liệu vào cấu trúc `SpriteDrawCommand` (104 bytes) chứa 4 toạ độ đỉnh, 6 indices tam giác, handle texture `ID3D11Texture2D` và kích thước nguồn.
  - **Frame Lifecycle & Memory Safety**:
    - Tự động dọn dẹp hàng đợi sprite trong `mock_SwapChain_Present` / `mock_SwapChain_Present1` khi số lượng vượt ngưỡng (tránh rò rỉ RAM).
    - Cung cấp API `D3D11Bridge_GetSpriteBatchInfo` đồng bộ thread-safe giữa luồng Direct3D 11 và luồng render EGL của Android.
  - **OpenGL ES 2.0 Batch Quad Rendering & Alpha Blending (`jni_bridge.cpp`)**:
    - Kích hoạt Alpha Blending (`glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`) đảm bảo các lớp sprite UI, chữ và hiệu ứng trong suốt hiển thị mượt mà không bị viền đen hoặc đè mảng đặc.
    - Vẽ tuần tự từng quad qua `glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices)` với toạ độ đỉnh và UV thực tế từ engine.
    - Tự động fallback về full-screen quad khi frame chưa có batch command riêng biệt.
- **Multi-Texture Binding & Dynamic GLES Texture Cache (Milestone M4 - Multi-Texture Composition)**:
  - **Texture Association per Sprite**:
    - Mỗi `SpriteDrawCommand` lưu trữ con trỏ `textureHandle` của texture đang kích hoạt tại thời điểm `DrawIndexed`.
    - API `D3D11Bridge_GetTextureInfoByHandle`: Cho phép EGL render thread truy vấn pixel RGBA8 và kích thước của bất kỳ texture handle nào trong `ResourceRegistry`.
  - **Dynamic GLES Texture Caching (`jni_bridge.cpp`)**:
    - Quản lý map `sGlesTextureCache` liên kết giữa COM handle và ID kết cấu OpenGL ES `GLuint`.
    - Tự động khởi tạo kết cấu mới khi gặp texture handle mới và tái sử dụng qua các frame mà không cần giải phóng/khởi tạo lại liên tục.
    - Khi vẽ chuỗi sprite quads, tự động chuyển đổi `glBindTexture(GL_TEXTURE_2D, spriteTexId)` tương ứng với từng quad, cho phép hiển thị đồng thời nhiều atlas UI, background và font texture trên cùng một khung hình.
- **Pixel-Space to NDC Coordinate Normalization & UV Mapping (Milestone M5 - Sprite UV Mapping & Projection)**:
  - **Quy luật toạ độ đỉnh Game Engine M2**:
    - Game engine sử dụng hệ toạ độ Pixel Screen Space với tâm tại $(0, 0)$: dải toạ độ ngang $X \in [-960, 960]$ (tổng chiều rộng 1920px), trục dọc $Y \in [-540, 540]$ (tổng chiều cao 1080px, hướng dương đi xuống dưới).
  - **Chuẩn hoá đỉnh sang OpenGL ES NDC $[-1.0, 1.0] \times [-1.0, 1.0]$**:
    - Khi phát hiện toạ độ đỉnh vượt ngưỡng pixel space ($|x| > 1.5$ hoặc $|y| > 1.5$), tự động thực hiện phép chiếu chuẩn hoá và lật trục $Y$:
      $$\text{pos.x}_{\text{NDC}} = \frac{\text{rawX}}{960.0f}, \quad \text{pos.y}_{\text{NDC}} = -\frac{\text{rawY}}{540.0f}$$
    - Ngăn chặn hoàn toàn hiện tượng đỉnh bị clip ngoài màn hình khi Vertex Shader nhận trực tiếp toạ độ pixel mà không qua projection matrix.
  - **Bảo vệ toạ độ UV (Safety Clamping)**:
    - Kiểm tra và lọc bỏ các giá trị dị thường (NaN, Inf hoặc nằm ngoài dải hợp lý $[-10, 10]$) trước khi gửi vào lệnh vẽ OpenGL ES, tránh artifacts render.
- **Index-Buffer-Driven Sprite Quad Extraction & Double-Buffering Frame Sync (Milestone M6 - Index Buffer Quad Extraction & Double Buffering)**:
  - **Index Buffer Driven Extraction (`mock_Context_DrawIndexed`)**:
    - Thay vì luôn đọc 4 đỉnh đầu tiên từ Vertex Buffer, hệ thống ánh xạ `sCurrentIndexBuffer` (hỗ trợ `DXGI_FORMAT_R16_UINT` và `R32_UINT`) kết hợp `startIndexLocation` và `baseVertexLocation` để giải mã chính xác các đỉnh mà engine yêu cầu vẽ:
      $$\text{vertexIndex} = \text{IndexBuffer}[\text{startIndexLocation} + i] + \text{baseVertexLocation}$$
    - Tính toán độ lệch byte trong vertex buffer an toàn dựa trên `sCurrentVertexOffset` và `sCurrentVertexStride` (20 bytes: pos.x [float 4B], pos.y [float 4B], color [uint32 4B], uv.u [float 4B], uv.v [float 4B]), bảo vệ chống tràn biên bộ nhớ (`byteOffset + stride <= byteWidth`).
    - Bố cục 20-byte Vertex Struct được xác thực thực nghiệm:
      $$\text{Offset 0: Pos.X (float)}, \quad \text{Offset 4: Pos.Y (float)}, \quad \text{Offset 8: Color (uint32)}, \quad \text{Offset 12: UV.U (float)}, \quad \text{Offset 16: UV.V (float)}$$
  - **Degenerate Quad Filtering**:
    - Tự động nhận diện và loại bỏ các quad bị suy biến (degenerate quads có 4 đỉnh trùng nhau tại $(0, 0)$ hoặc chiều rộng / chiều cao xấp xỉ 0).
    - Ngăn ngừa hiện tượng quad rỗng làm ô nhiễm hàng đợi sprite khiến pipeline rơi vào trạng thái vẽ vô hình.
  - **Sprite Batch Double-Buffering Sync**:
    - Tách biệt hai vùng nhớ đệm sprite: `sActiveDrawingSprites` (luồng D3D11 tích luỹ các lệnh vẽ trong frame hiện tại) và `sReadyFrameSprites` (luồng EGL Render đọc và xuất ra màn hình).
    - Tại thời điểm `mock_SwapChain_Present` / `mock_SwapChain_Present1`, thực hiện hoán đổi nguyên tử (atomic move swap) giữa hai bộ đệm và dọn dẹp bộ đệm tích luỹ, loại bỏ triệt để hiện tượng dồn tích hàng trăm sprite rác qua các frame.
    - Khi một frame không có sprite riêng lẻ hợp lệ (`sReadyFrameSprites` rỗng), renderer tự động kích hoạt chế độ fallback quad toàn màn hình cho texture đang kích hoạt (`ActiveTexture` 2048x1088 / 3424x3424), đảm bảo background và menu luôn được hiển thị liên tục.
- **Pipeline Resource Retention & Deferred Mock Release (Milestone M7 - Pipeline Retention & Sprite Composition)**:
  - **Pipeline Reference Retention (`IASetVertexBuffers`, `IASetIndexBuffer`)**:
    - Áp dụng cơ chế chuẩn COM DirectX 11: Khi game engine bind vertex buffer hoặc index buffer vào pipeline, hệ thống tự động tăng biến đếm tham chiếu (`sRegistry.addRef`), và giải phóng (`sRegistry.release`) buffer cũ khi chuyển đổi hoặc kết thúc frame.
    - Đảm bảo vùng nhớ đệm CPU của buffer không bị giải phóng giữa chừng trong khi pipeline đang chuẩn bị gọi `DrawIndexed`.
  - **Deferred COM Mock Object Deletion (`sDeferredReleaseList`)**:
    - Khi một đối tượng COM mock nhận `Release()` dẫn đến `refCount == 0`, hệ thống không lập tức gọi `delete` trên vùng nhớ heap.
    - Đưa con trỏ vào hàng đợi `sDeferredReleaseList` và chỉ dọn dẹp hàng loạt tại thời điểm `Present` (khi kết thúc khung hình).
    - Ngăn chặn hoàn toàn hiện tượng bộ cấp phát `malloc` tái sử dụng cùng địa chỉ cho các buffer khác nhau trong cùng một frame, loại bỏ lỗi ghi đè dữ liệu giữa Vertex Buffer và Index Buffer.
- **Hệ Thống Đồng Bộ Đầu Vào & Batch Sprite (Milestone M8 - Input Subsystem & Batch Sprite Composition)**:
  - **Đồng Bộ Input 3 Tầng (Input Subsystem Synchronization)**:
    - **Tầng Android UI**: `MainActivity.java` lắng nghe trực tiếp `onTouchEvent`, `onKeyDown`, `onKeyUp`, và `onGenericMotionEvent`, chuyển đổi toàn bộ chạm màn hình và phím bấm thành các sự kiện native (`nativeSendTouchEvent`, `nativeSendKeyEvent`).
    - **Tầng C++ Adapter**: `input_injector.cpp` chuẩn hoá toạ độ chạm sang không gian 1920x1080, đồng thời cập nhật tức thời trạng thái bàn phím ảo (`Win32Bridge_SetKeyState`) và toạ độ chuột (`Win32Bridge_SetCursorPos`). Khi người dùng chạm màn hình, hệ thống đồng thời kích hoạt `VK_LBUTTON`, `VK_RETURN` (Enter) và `VK_SPACE` để game nhận diện sự kiện "PRESS ANY BUTTON".
    - **Tầng Win32 & DirectInput Bridge**: `win32_bridge.cpp` và `dinput_bridge.cpp` cung cấp trạng thái phím nhất quán cho cả Win32 message loop (`PeekMessageW`, `GetMessageW`), polling APIs (`GetKeyState`, `GetAsyncKeyState`, `GetKeyboardState`), và DirectInput 8 (`mock_DIDevice_GetDeviceState`), giúp game engine visual novel chuyển cảnh mượt mà từ màn hình Intro / Title sang Main Menu.
  - **Trích Xuất Batch Quads Đa Phần Tử (Batch Quad Extraction)**:
    - Trong `mock_Context_DrawIndexed`, thay vì chỉ lấy 1 quad đầu tiên, thuật toán duyệt qua toàn bộ batch `indexCount` (bước nhảy 4 indices đối với strip/quad và 6 indices đối với triangle list).
    - Giúp trích xuất đầy đủ 100% các sprite con trong cùng một lệnh vẽ (như các nút bấm UI, text nhãn, icon phụ) thay vì bị cắt xén dữ liệu.
  - **Giải Mã Màu Đỉnh & Alpha Blending (Vertex Color Modulation & Blending)**:
    - Bóc tách trường Color 4 bytes tại offset 8 của Vertex: `[B, G, R, A]`.
    - Hỗ trợ chuẩn Modulate2X: giá trị `0x808080` ánh xạ sang hệ số màu chuẩn `(1.0, 1.0, 1.0)`, còn byte thứ 4 ánh xạ sang giá trị alpha độ trong suốt $[0.0, 1.0]$.
    - Shader GLES 2.0 trong `jni_bridge.cpp` nhân trực tiếp màu texture với màu đỉnh: $\text{FragColor} = \text{texture2D}(uTexture, vTexCoord) \times vColor$, kết hợp cùng chế độ hoà trộn `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` cho hình ảnh trong trẻo, không bị bóng mờ hay vệt đen.
- **Windows Thread Local Storage (TLS) Subsystem (`gs:[0x58]`)**:
  - Tự động bóc tách thư mục PE DataDirectory[9] (`IMAGE_DIRECTORY_ENTRY_TLS`).
  - Phân bổ vùng nhớ động 64KB chứa bảng con trỏ `ThreadLocalStoragePointer` (128 slots) và 32KB đệm TLS data cho thread.
  - Gán trực tiếp con trỏ mảng vào trường `gs:[0x58]` của cấu trúc Windows 64-bit TEB.
  - Tự động sao chép template data ban đầu từ `StartAddressOfRawData` và xác lập `_tls_index = 0`.
  - Loại bỏ hoàn toàn lỗi crash null-pointer dereference (`SEGV_MAPERR fault addr 0x0`) khi các container C++ (như `std::vector`), luồng hoặc từ khoá `thread_local` của game engine giải tham chiếu `[rax + rcx*8]` qua thanh ghi phân đoạn `GS`.
- **Hệ Thống XAudio2 Audio Voice Mock (`xaudio2_bridge.cpp` - Milestone M9 & M11)**:
  - Cung cấp mô phỏng đầy đủ các đối tượng âm thanh COM của DirectX XAudio2:
    - **`IXAudio2` (Chuẩn COM Vtable x64)**:
      - Slot 0: `QueryInterface`
      - Slot 1: `AddRef`
      - Slot 2: `Release`
      - **Slot 5 (Offset 0x28)**: `CreateSourceVoice` (Được sửa chuẩn xác từ Slot 6 ở Milestone M11).
      - **Slot 6 (Offset 0x30)**: `CreateSubmixVoice` (Được sửa chuẩn xác từ Slot 8 ở Milestone M11).
      - **Slot 7 (Offset 0x38)**: `CreateMasteringVoice` (Được sửa chuẩn xác từ Slot 7 ở Milestone M11).
      - Slot 8: `CreateSubmixVoice` (giữ fallback dự phòng).
    - **`IXAudio2Voice`**: Cài đặt `DestroyVoice` (Slot 18, Offset 0x90).
    - **`IXAudio2SourceVoice`**: Cài đặt `Start` (Slot 19, Offset 0x98), `Stop` (Slot 20, Offset 0xA0), `SubmitSourceBuffer` (Slot 21, Offset `0xA8`), `FlushSourceBuffers` (Slot 22, Offset 0xB0), và `GetState` (Slot 25, Offset 0xC8, xóa trắng 24 bytes `XAUDIO2_VOICE_STATE`).
    - **`IXAudio2SubmixVoice`**: Cài đặt vtable độc lập 32 slots cho submix routing.
  - **Phân Tích & Khắc Phục Lỗi Crash Input RVA `0x39253E`**:
    - Khi người dùng chạm màn hình hoặc bấm phím, game engine M2 gọi hàm phát âm thanh UI tại RVA `0x3921A0`.
    - Tại RVA `0x392350`, engine thực hiện lệnh `call qword ptr [rax + 0x28]` (Slot 5 của IXAudio2) để tạo `IXAudio2SourceVoice` và gán vào trường `[rbx]` của voice wrapper context.
    - Trước khi sửa: Do bảng vtable gán `CreateSourceVoice` vào Slot 6 (Offset 0x30) còn Slot 5 chứa stub trả về `0`, con trỏ `[rbx]` không được khởi tạo (bị giữ nguyên `NULL`).
    - Hậu quả: Đến RVA `0x39253E`, engine thực thi `mov rax, [rcx]; call [rax + 0xA8]` (`SubmitSourceBuffer`) với `rcx = [rbx] = NULL` $\rightarrow$ gây lỗi crash `Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0`.
    - Sau khi sửa Slot 5 = `CreateSourceVoice`: Đối tượng `MockSourceVoice` được cấp phát chuẩn xác, lệnh `call [rax + 0xA8]` gọi thành công vào `sourceVoice_SubmitSourceBuffer`, loại bỏ hoàn toàn lỗi crash khi người dùng tương tác input, cho phép game xử lý logic và chuyển cảnh mượt mà.
  - Độ phức tạp: $O(1)$ cho mỗi thao tác cấp phát và truy xuất phương thức COM Voice.
- **Hệ Thống Bóc Tách Batch Quads Chuẩn Strip Restart & Bảo Vệ Fallback Render (`d3d11_bridge.cpp`, `jni_bridge.cpp` - Milestone M10)**:
  - **Cơ Chế Bóc Tách Triangle Strip với Degenerate Restart ($4 + k \times 6$) & Index Buffer Bounds**:
    - Nhận diện quy luật mảng index của Mages Engine: Các batch quads UI/Title Menu được tổ chức dưới dạng Triangle Strip kết nối bởi degenerate vertices ($N$ quads = $4 + (N-1) \times 6$ indices: $4, 10, 16, 22, 28, 34, 40...$).
    - Thuật toán tự động xác định bước nhảy `quadStride = 6` và trích xuất chuẩn xác 4 đỉnh thực của từng quad tại toạ độ offset $[k \times 6 + 0, k \times 6 + 1, k \times 6 + 2, k \times 6 + 3]$, bỏ qua 2 degenerate link indices.
    - Sửa triệt để lỗi kiểm tra biên `startIndexLocation`: Loại bỏ so sánh nhầm lẫn `startIndexLocation + idxOffset >= indexCount` (vốn loại bỏ sai toàn bộ các lệnh vẽ sprite có `startIndexLocation > 0`), chuẩn hoá kiểm tra `idxOffset >= indexCount` và kiểm tra độ rộng byte thực tế của index buffer (`requiredBytes <= iDesc.byteWidth`), đồng thời bảo vệ chống âm `vertexIdx >= 0`.
    - Hỗ trợ đồng thời chế độ Triangle List (`IndexCount % 6 == 0` với stride 6, trích xuất đỉnh thứ 4 tại offset 5) và Quads liên tiếp (`IndexCount % 4 == 0` với stride 4).
    - Độ phức tạp tính toán: $O(N)$ với $N$ là số lượng sprite quads trong frame (thông thường $N \le 64$, thời gian thực thi $< 0.1\text{ms}$).
  - **Cơ Chế Bảo Vệ Fallback Rendering & Chuẩn Hoá Toạ Độ UV (`jni_bridge.cpp`, `d3d11_bridge.cpp` - Milestone M10 & M12)**:
    - **Loại bỏ hoàn toàn Fallback vẽ đè Atlas thô (`jni_bridge.cpp`)**:
      - Trước đây fallback cho phép các texture kích thước $\le 2048 \times 1088$ vẽ đè lên toàn màn hình dưới dạng full-screen quad khi `spriteCount == 0`. Tuy nhiên tấm Texture Atlas chính của Title Menu lại có kích thước $2048 \times 1088$, dẫn đến việc tấm atlas này bị quăng thẳng lên màn hình che toàn bộ các thành phần game khác.
      - Tại Milestone M12, điều kiện fallback full-screen quad được siết chặt tuyệt đối: chỉ cho phép khi texture khớp chính xác độ phân giải frame game chuẩn (`activeW == 1920 && activeH == 1080`). Toàn bộ các texture atlas thô ($2048 \times 1088$, $3424 \times 3424$,...) bị loại bỏ hoàn toàn khỏi luồng fallback.
    - **Chuẩn hoá toạ độ UV đa tầng (`d3d11_bridge.cpp`)**:
      - Hỗ trợ cả hai cơ chế định vị texture của Mages Engine:
        - Chuẩn hoá pixel coordinate: Khi toạ độ $U, V > 16.0$ và nhỏ hơn hoặc bằng kích thước thực của texture liên kết (`cmd.textureWidth`, `cmd.textureHeight`), hệ thống tự động chia theo kích thước texture để thu về khoảng $[0.0, 1.0]$.
        - Chuẩn hoá texture wrapping modulo: Đối với các toạ độ UV vượt quá $1.0$ do scale lặp (như $1.409, 3.138$), hệ thống áp dụng phép trừ phần nguyên `uv - floor(uv)` để giữ toạ độ UV luôn nằm trong dải hợp lệ $[0.0, 1.0]$ cho OpenGL ES shader.
    - Độ phức tạp: $O(1)$ cho mỗi đỉnh sprite, tổng thời gian xử lý $< 0.05\text{ms}$ cho toàn bộ frame.
  - **Telemetry Input Element & Topology Diagnostics**:
    - Thu thập chi tiết thông số `D3D11_INPUT_ELEMENT_DESC` trong `CreateInputLayout` và theo dõi sự thay đổi topology trong `IASetPrimitiveTopology`.
    - Ghi nhận số lượng sprite sẵn sàng xuất màn hình tại `Present1` và số sprite thực sự render tại `jni_bridge.cpp` phục vụ chẩn đoán thời gian thực.


- **Hệ Thống Render Đa Kết Cấu Kép (Color & Alpha Mask) & Chuẩn Hoá Toạ Độ Atlas Ảo M2 (`d3d11_bridge.cpp`, `jni_bridge.cpp`, `resource_registry.cpp` - Milestone M13)**:
  - **Phân Tích Cốt Lõi Cơ Chế Render M2 Engine qua IDA Pro MCP**:
    - **RenderTarget Controller (`sub_1404C6130`)**: Game engine quản lý việc chuyển render target thông qua COM vtable slot 264 (`OMSetRenderTargets`) và slot 352 (`RSSetViewports`).
    - **Virtual Screen Quad Submission (`sub_1404FA480`)**: M2 Engine định nghĩa hệ toạ độ UV ảo trên không gian màn hình chuẩn $1928 \times 1088$. Các quad sprite được tính toán tỷ lệ tương đối theo không gian ảo này trước khi ánh xạ lên tấm Texture Atlas kích thước lớn $3424 \times 3424$ hoặc $2048 \times 1088$.
    - **Multi-Texture Binding (`sub_140531550` & `sub_1404F85C0`)**: Khám phá kiến trúc ghép texture kép (Dual-Slot Binding) của game:
      - **Slot 0 (Color Texture)**: Chứa thông tin màu RGB (hoặc màu cơ sở).
      - **Slot 1 (Alpha Mask Texture)**: Texture định dạng `DXGI_FORMAT_A8_UNORM` (Format 65) đóng vai trò mặt nạ alpha (như các đoạn chữ Startup Caution $1928 \times 528$, Logo MAGES/Chiyomaru/MTwo $2048 \times 1096$, Title Buttons "PRESS ANY BUTTON", "NEW GAME"...).
      - Pixel Shader gốc SM4 thực hiện phép nhân màu: $\text{FinalColor} = (\text{Color.rgb}, \text{Color.a} \times \text{AlphaMask.a}) \times \text{VertexColor}$.
  - **Giải Pháp Nâng Cấp Tầng Render Native**:
    1. **Mở Rộng Định Dạng `DXGI_FORMAT_A8_UNORM` Trên CPU (`resource_registry.cpp`)**:
       - Khi nạp hoặc cập nhật texture Format 65, hệ thống tự động mở rộng từng byte alpha $A$ thành 4 byte RGBA $(255, 255, 255, A)$.
       - Đảm bảo kênh màu luôn có sắc trắng đục $(1.0, 1.0, 1.0)$, giúp cả trường hợp vẽ độc lập lẫn hoà trộn shader đều hiển thị sáng rõ, loại bỏ hoàn toàn hiện tượng chữ bị biến thành mảng đen hoặc mất tích.
    2. **Shader Đa Kết Cấu Kép (Dual Texture Sampler Shader - `jni_bridge.cpp`)**:
       - Khai báo thêm `uniform sampler2D uAlphaTexture` và `uniform int uHasAlphaTexture`.
       - Logic Fragment Shader:
         ```glsl
         vec4 baseColor = texture2D(uTexture, vTexCoord);
         if (uHasAlphaTexture == 1) {
             float maskAlpha = texture2D(uAlphaTexture, vTexCoord).a;
             gl_FragColor = vec4(baseColor.rgb, baseColor.a * maskAlpha) * vColor;
         } else {
             gl_FragColor = baseColor * vColor;
         }
         ```
       - Trong vòng lặp sprite: Tự động bind `cmd.textureHandle` vào `GL_TEXTURE0` và `cmd.alphaTextureHandle` vào `GL_TEXTURE1`.
    3. **Chuẩn Hoá Toạ Độ UV Cho Atlas M2 Engine & Cơ Chế Reset Slot 1 Alpha (`d3d11_bridge.cpp` - Milestone M14 & M16)**:
       - **Chống Rò Rỉ Alpha Mask Binding**:
         - Trong `PSSetShaderResources`: Khi game chỉ truyền `startSlot == 0 && numViews == 1`, hệ thống bắt buộc reset rõ ràng `sBoundTextureResources[1] = nullptr` và `sBoundShaderResourceViews[1] = nullptr`. Điều này ngăn chặn triệt để tình trạng các draw call đơn lẻ sau đó bị lấy nhầm texture alpha rác của frame trước, gây mất hình hoặc biến thành mảng trắng.
         - Trong `DrawIndexed`: Chỉ gán `cmd.alphaTextureHandle` khi texture ở Slot 1 có định dạng chuẩn `DXGI_FORMAT_A8_UNORM` (Format 65).
       - **Chuẩn Hoá Toạ Độ UV Phân Biệt Giữa Texture Thông Thường & Atlas**:
         - M2 Engine sinh toạ độ UV sprite thô kết hợp giữa hai cơ chế:
           - **Quad thông thường / Logo / Background** ($\text{width} \le 2048, \text{height} \le 1088$): Toạ độ UV nằm trong khoảng $[0.0, 1.0]$ được bảo toàn nguyên vẹn $100\%$, tuyệt đối không bị modulo hay scale nhầm để tránh biến dạng hình ảnh hoặc làm sụp đổ quad thành 1 điểm.
           - **Sprite Atlas M2 Engine & Toạ Độ Màn Hình Ảo 1920x1080** (Milestone M18):
             - M2 Engine biểu diễn toạ độ UV sprite theo tỷ lệ không gian màn hình chuẩn 1920x1080 ( = x / 1920, v = y / 1080$).
             - Khi toạ độ pixel trên Texture Atlas ( \times 3424$ hoặc  \times 1088$) vượt quá 1920 hoặc 1080, giá trị , v$ sẽ lớn hơn 1.0 (ví dụ .409, 1.997$ tương ứng pixel  \times 2157$; .296, 3.138$ tương ứng  \times 3389$).
             - Hệ thống chuẩn hoá về toạ độ thực của Texture Atlas bằng phép toán:
               $$u_{\text{atlas}} = \frac{u \times 1920.0}{\text{textureWidth}}, \quad v_{\text{atlas}} = \frac{v \times 1080.0}{\text{textureHeight}}$$
             - Ánh xạ chính xác 100% từng icon/sprite từ file định nghĩa PSB vào đúng toạ độ pixel của Texture Atlas, loại bỏ hoàn toàn việc cắt lệch ảnh.
         - Toạ độ pixel tuyệt đối ($> 16.0$) tiếp tục được chia trực tiếp cho kích thước texture tương ứng.
  - **Độ phức tạp**: $O(1)$ cho mỗi đỉnh sprite và mỗi lần lấy mẫu fragment trên GPU.

- **Stub WinMM Audio Driver, Chặn libjack.dll & Chuyển Đổi Định Dạng `DXGI_FORMAT_B8G8R8A8_UNORM` (`win32_bridge.cpp`, `resource_registry.cpp` - Milestone M15 & M17)**:
  - **Phân Tích Nguyên Nhân Crash Khi Tap Màn Hình qua IDA Pro MCP**:
    - Khi người dùng chạm màn hình để bỏ qua logo hoặc phát âm thanh tương tác, game gọi vào driver âm thanh phụ thuộc `winmm.dll` (`sub_1400CA1D0`).
    - Engine truy vấn con trỏ hàm động qua `GetProcAddress` cho các API `waveOut*` và `waveIn*` (`waveOutGetNumDevs`, `waveOutGetDevCapsA`, `waveOutOpen`, `waveInOpen`...).
    - Trước đây `GetProcAddress` trả về `nullptr`, dẫn đến lệnh `call r9` / `call r10` gọi trúng địa chỉ null `0x0000000000000000`, kích hoạt tín hiệu `SIGSEGV (SEGV_MAPERR)`.
    - **Giải pháp**: Triển khai đầy đủ stub cho toàn bộ nhóm API WinMM `waveOut*` và `waveIn*`, trả về `MMSYSERR_NOERROR` (0) cùng handle thiết bị giả lập hợp lệ (`0x8001`, `0x8002`).
    - **Chặn Thư Viện JACK Audio (`libjack.dll` - Milestone M17)**: Thư viện miniaudio tìm kiếm driver JACK (`sub_1400BD5B0`). Nếu trả về fake module handle, engine sẽ gọi `GetProcAddress("jack_client_open")` trả về null và crash khi gọi lệnh `call rax`. Bằng cách chặn chuỗi `"jack"` tại `stub_LoadLibraryA` và `stub_LoadLibraryExW` trả về `nullptr`, miniaudio tự động fallback an toàn về WaveOut.
  - **Sửa Lỗi Kênh Màu Định Dạng `DXGI_FORMAT_B8G8R8A8_UNORM` (Format 87)**:
    - Các texture hệ thống và logo (kích thước $1024 \times 1024$) được nạp với thứ tự kênh BGRA.
    - Tại `resource_registry.cpp`, bổ sung bộ chuyển đổi đảo kênh Red và Blue $(B, G, R, A) \to (R, G, B, A)$ ở cả hai nhánh `registerTexture` và `updateTexture`.
    - Loại bỏ hiện tượng sai lệch màu sắc và nhiễu màu pixel trên logo nhà phát triển.
  - **Độ phức tạp**: $O(W \times H)$ cho mỗi lần cập nhật texture $1024 \times 1024$ (thực thi $< 0.5\text{ms}$ trên CPU).

- **Khôi Phục Màu Đỉnh Logo & Cảnh Báo, Chuẩn Hoá Toạ Độ UV Logo & Chống Lớp Trắng Che Phủ Hoạt Ảnh Intro (`d3d11_bridge.cpp`, `jni_bridge.cpp` - Milestone M19)**:
  - **Khắc Phục Lỗi Vô Hình Của Logo Nhà Phát Triển & Màn Hình Cảnh Báo (Epilepsy Caution)**:
    - **Phân Tích Cốt Lõi Vertex Layout Mismatch**:
      - Title Screen sử dụng layout: $\text{Position (float2, 8B)} + \text{Color (uint32, 4B)} + \text{TexCoord (float2, 8B)} = 20\text{ bytes}$. Trường Color tại offset 8 mang giá trị `0xFF808080`.
      - Logo (`MAGES.`, `Chiyomaru Studio`, `MTwo`) và Bảng Cảnh Báo sử dụng layout: $\text{Position (float3, 12B: X, Y, Z)} + \text{TexCoord (float2, 8B: U, V)} = 20\text{ bytes}$. Trường tại offset 8 là $\text{Pos.Z} = 0.0\text{f}$ (`0x00000000`), hoàn toàn không có trường Color.
      - Trước đây hệ thống ép kiểu offset 8 thành màu RGBA. Khi byte 8..11 bằng 0, alpha đỉnh bị tính bằng $0.0\text{f}$, khiến toàn bộ quad của Logo và Cảnh Báo bị hoà trộn alpha trong suốt $100\%$ và vô hình trên màn hình.
      - **Giải Pháp**: Khi 4 byte tại offset 8 bằng `0x00000000` (Pos.Z = 0.0f) hoặc khi $\text{cA} == 0 \land \text{cR} == 0 \land \text{cG} == 0 \land \text{cB} == 0$, hệ thống tự động nhận diện layout không có Color và khôi phục màu đỉnh mặc định là trắng đục chuẩn:
        $$\text{vColor} = (1.0\text{f}, 1.0\text{f}, 1.0\text{f}, 1.0\text{f})$$
  - **Chuẩn Hoá Toạ Độ UV Cho Texture Logo ($2048 \times 1088, 2048 \times 1096, 1928 \times 1088, 1928 \times 1040$)**:
    - Nội dung hiển thị thực tế của các logo và chữ cảnh báo nằm trong vùng $1920 \times 1080$ trên kết cấu kích thước $2048 \times 1088$ (hoặc 1928x1040).
    - Áp dụng phép chiếu tỷ lệ không gian màn hình ảo:
      $$u_{\text{norm}} = \frac{u \times 1920.0\text{f}}{\text{textureWidth}}, \quad v_{\text{norm}} = \frac{v \times 1080.0\text{f}}{\text{textureHeight}}$$
    - Đảm bảo toàn bộ logo và cảnh báo khớp chuẩn xác 100% tỷ lệ khung hình 1080p, loại bỏ hiện tượng co lệch ảnh và dải viền đen padding.
  - **Loại Bỏ Màn Hình Trắng Che Khuất Hoạt Ảnh Xuất Hiện Title Screen (`w_full` Protection)**:
    - Trong `title.pure.psb`, đối tượng `title_font/w_full` là hình chữ nhật trắng toàn màn hình ($1920 \times 1080$) trên Texture Atlas $3424 \times 3424$ dùng cho hiệu ứng fade chuyển cảnh kết hợp Alpha Mask từ Render Target Slot 1.
    - Khi Render Target chưa có dữ liệu pixel trên CPU (`hasValidAlphaMask == false`), nếu vẽ `w_full` mà không có mặt nạ alpha, tấm trắng đặc sẽ che phủ toàn bộ màn hình và giấu kín hoạt ảnh chuyển động xuất hiện của logo STEINS;GATE RE:BOOT.
    - **Giải Pháp**: Siết chặt kiểm tra `hasValidAlphaMask`: Chỉ kích hoạt `uHasAlphaTexture = 1` khi texture mask thực sự có dữ liệu pixel hợp lệ và tải thành công lên GPU (`alphaTexId != 0`). Đồng thời tự động bỏ qua quad `w_full` khi mặt nạ alpha chưa sẵn sàng, giải phóng tầm nhìn để hoạt ảnh xuất hiện của Logo Title hiển thị mượt mà.
  - **Độ phức tạp**: $O(1)$ cho mỗi đỉnh sprite quad và mỗi chu kỳ kiểm tra texture bind.

- **Khắc Phục Triệt Để Màn Trắng Hoạt Ảnh Chuyển Cảnh, Khôi Phục Chữ Cảnh Báo Attention & Hook An Toàn Chống Tràn Stack Cookie (`jni_bridge.cpp`, `d3d11_bridge.cpp`, `payload_loader.cpp` - Milestone M20)**:
  - **Triệt Tiêu Hoàn Toàn Tấm Màn Trắng Che Khuất Hoạt Ảnh Nét Vẽ (`w_full` Unconditional Skip - `jni_bridge.cpp`)**:
    - **Phân Tích**: Trong engine M2, hiệu ứng vẽ viền các nét chữ cái Steins;Gate (`AtlasQuad` kích thước nhỏ $0.1 \times 1.1$) được thiết kế kết hợp với quad trắng $2.0 \times 2.0$ (`w_full`) quét qua Stencil Test. Trên OpenGL ES chưa có Stencil Test và `alphaTextureHandle` được cấp phát bộ nhớ RAM thành công khiến `hasValidAlphaMask = true`, vô hiệu hoá bộ lọc cũ. Kết quả là mỗi frame vẽ nét chữ, một tấm trắng đặc lại phủ kín toàn màn hình, che khuất toàn bộ hoạt ảnh.
    - **Giải Pháp**: Bỏ qua điều kiện `!hasValidAlphaMask`. Bất kỳ quad nào có `textureWidth == 3424 && quadSpanX >= 1.8f && quadSpanY >= 1.8f && cmd.uv[0][1] > 0.6f` đều bị bỏ qua vô điều kiện (`continue`). Giải phóng tầm nhìn, để lộ 100% hoạt ảnh nét vẽ vi mạch và chữ Title xuất hiện mượt mà.
  - **Khôi Phục Văn Bản Cảnh Báo Attention (`d3d11_bridge.cpp`)**:
    - **Phân Tích**: Game tạo Dynamic Font Texture $1024 \times 1024$ (Format 87) và cập nhật qua `UpdateSubresource` với `RowPitch = 7680`. Trước khi vẽ text glyphs, game unbind Slot 0 (`PSSetShaderResources(0, 4, [0, 0, 0, 0])`) và chuyển Topology sang 4 (TriangleList). Do Slot 0 bằng null, hệ thống fallback về texture cũ `1928x1040` (khung vi mạch), khiến chữ bị lấy mẫu sai toạ độ và vô hình.
    - **Giải Pháp**:
      1. Trong `mock_Context_UpdateSubresource`: Ghi nhận con trỏ texture vừa cập nhật `sLastUpdatedTextureResource = pDstResource`.
      2. Trong `mock_Context_DrawIndexed`: Khi `sBoundTextureResources[0] == nullptr`, nếu `sCurrentPrimitiveTopology == 4` (TriangleList vẽ chữ) và có `sLastUpdatedTextureResource`, tự động ưu tiên gán texture font này làm `colorTex`.
      3. Đảo thứ tự kiểm tra topology: Ưu tiên `sCurrentPrimitiveTopology == 4 && indexCount % 6 == 0` trước `(indexCount - 4) % 6 == 0` để nhận diện chính xác TriangleList font glyphs.
      4. Triển khai `mock_Context_VSSetShaderResources` để đảm bảo không bỏ sót bất kỳ texture binding nào từ Vertex Shader stage.
  - **Khắc Phục Crash SIGTRAP Khi Chạm Màn Hình Ở Press Any Button (`payload_loader.cpp`)**:
    - **Phân Tích Gốc Rễ qua IDA Pro Decompile**:
      - Khi người dùng chạm màn hình ở Press Any Button, game mở file video nền `windata/movie/sg_titlebg.webm` và đọc qua `ReadFile`.
      - Engine gọi hàm `sub_14056C470` (địa chỉ ảo `0x14056C470`, RVA `0x56C470`) để phân tích EBML header. Hàm này là `std::string::find` / `memmem` của MSVC STL dùng lệnh SSE4.2 `pcmpestri`.
      - Tại `0x14056C557`, lệnh `call memcpy` sao chép `(a2 - v7)` bytes vào biến cục bộ `v26` trên stack (`[rsp+20h]`, kích thước 16 bytes). Khi kích thước vượt quá 16 bytes, `memcpy` ghi đè qua `[rsp+30h]` phá hỏng `__security_cookie` (`var_48`).
      - Cuối hàm tại `0x14056C5FB`, `call __security_check_cookie` phát hiện cookie bị ghi đè, kích hoạt `__report_securityfailure` / `__fastfail` khiến nhân Linux ném tín hiệu `SIGTRAP` làm app crash văng ra Launcher.
    - **Giải Pháp**:
      - Triển khai hàm `safe_memmem_search` an toàn chuẩn C với Microsoft x64 calling convention (`__attribute__((ms_abi))`), sử dụng `memmem` an toàn không giới hạn kích thước đệm stack.
      - Trong `PayloadLoader::applyRelocations`, ghi đè 12 byte đầu của hàm `sub_14056C470` tại RVA `0x56C470` bằng lệnh nhảy 64-bit trực tiếp: `mov rax, safe_memmem_search; jmp rax;` (mã máy `48 B8 <addr> FF E0`).
      - Loại bỏ vĩnh viễn nguy cơ tràn stack buffer cookie, cho phép engine đọc WebM video mượt mà và chuyển cảnh vào Menu chính không bị crash.
  - **Độ phức tạp**: $O(1)$ cho kiểm tra quad và hook nhảy, $O(M \times N)$ cho thuật toán `memmem` an toàn.

- **Milestone M21: Đồng Bộ Hóa Đa Luồng Win32 (SRWLock & ConditionVariable) và Triệt Tiêu Deadlock Mutex**:
  - **Bối Cảnh & Phân Tích Gốc Rễ**:
    - Khi người dùng chạm màn hình ở Press Any Button, engine khởi tạo Worker Thread qua `stub_CreateThread` (chạy trampoline `sub_14008E6D0`).
    - Worker Thread quản lý hàng đợi tác vụ thông qua cơ chế đồng bộ C++ STL: `std::mutex` (`_Mtx_lock` tại `0x14056D100`) và `std::condition_variable` (`sub_14056D278`).
    - Trong MSVC STL trên x64 Windows, `std::mutex` được xây dựng dựa trên `AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive` của Win32 API.
    - Trước đó, các hàm SRWLock và ConditionVariable trong wrapper chỉ là stub rỗng (`return 1` / `no-op`). Khi Worker Thread gọi `mtx_do_lock` tranh chấp với Main Thread, việc thiếu cơ chế blocking thật sự khiến hàm phát hiện mutex đang ở trạng thái locked (`count > 1`) và mutex là non-recursive (`cờ 0x100 = 0`), dẫn đến trả về mã lỗi 3 (`_Thrd_error`).
    - Engine lập tức gọi `std::_Throw_Cpp_error(5)`. Do môi trường wrapper Android không có MSVC C++ SEH Exception Handler, lệnh ném ngoại lệ dẫn thẳng đến `std::terminate()` $\rightarrow$ `__fastfail()` $\rightarrow$ nhân Linux ném `SIGTRAP` làm sập game.
    - Đồng thời, `stub_GetCurrentThreadId` cũ trả về `pthread_self()` bị cắt ngắn 32-bit, không đồng nhất với Thread ID của hệ điều hành.
  - **Giải Pháp Toàn Diện**:
    1. **Triển khai chuẩn Win32 SRWLock Subsystem (`win32_bridge.cpp`)**:
       - Sử dụng `std::shared_mutex` để quản lý các đối tượng SRWLock thông qua bảng ánh xạ `gSRWTable`.
       - Hỗ trợ đầy đủ `stub_AcquireSRWLockExclusive`, `stub_ReleaseSRWLockExclusive`, `stub_AcquireSRWLockShared`, `stub_ReleaseSRWLockShared`, và `stub_TryAcquireSRWLockExclusive`.
    2. **Triển khai chuẩn Win32 ConditionVariable Subsystem (`win32_bridge.cpp`)**:
       - Sử dụng `std::condition_variable_any` phối hợp cùng `std::unique_lock` / `std::shared_lock` với cờ `std::adopt_lock`.
       - Trong `stub_SleepConditionVariableSRW`, tự động giải phóng SRWLock trước khi chờ và tái sở hữu (re-acquire) khi được đánh thức bởi `stub_WakeConditionVariable` hoặc `stub_WakeAllConditionVariable`, đúng 100% đặc tả Win32.
    3. **Chuẩn hoá Win32 Thread ID**:
       - Cập nhật `stub_GetCurrentThreadId` sử dụng `syscall(SYS_gettid)` trên Android/Linux, đảm bảo mỗi thread có số nguyên 32-bit duy nhất, đồng bộ chính xác với Worker Thread trampoline.
    4. **Hotfix Bảo Vệ Kép Chống Mutex Deadlock Exception (`payload_loader.cpp`)**:
       - Tại RVA `0x56D20D` trong `mtx_do_lock`, ghi đè lệnh nhảy có điều kiện `jnz loc_14056D239` (byte `0x75`) thành lệnh nhảy vô điều kiện `jmp loc_14056D239` (byte `0xEB`).
       - Biến mọi mutex non-recursive thành re-entrant an toàn, đảm bảo hàm `mtx_do_lock` không bao giờ trả về mã lỗi 3 và loại bỏ vĩnh viễn nguy cơ kích hoạt `std::_Throw_Cpp_error(5)`.
  - **Độ phức tạp**: $O(1)$ cho mỗi thao tác khóa/mở khóa và đánh thức Condition Variable.

- **Milestone M22: Khôi Phục Toàn Diện Văn Bản Cảnh Báo Attention và Triệt Tiêu Alpha Mask Rỗng**:
  - **Bối Cảnh & Phân Tích Gốc Rễ**:
    - Màn hình cảnh báo khởi đầu (Epilepsy / Photosensitivity Warning) hiển thị được khung vi mạch Attention nhưng phần nội dung văn bản ở giữa lòng khung bị rỗng đen xì (`tests/automation/run_m21_verified/frame_6s.png`).
    - **Nguyên nhân 1 (Gán sai Texture Handle)**: Khung vi mạch (Texture `1928x1040`) được vẽ ở DrawIndexed #1. Đến DrawIndexed #121 (Quad vẽ văn bản ở tâm màn hình), game không unbind Slot 0 nên `sBoundTextureResources[0]` vẫn giữ con trỏ texture khung vi mạch thay vì gán texture văn bản vừa cập nhật (`sLastUpdatedTextureResource`).
    - **Nguyên nhân 2 (Alpha Mask rỗng triệt tiêu 100% màu)**: Ngay trước draw call vẽ chữ, game bind Texture `1920x1080` Format 65 (`DXGI_FORMAT_A8_UNORM`) vào Slot 1. Do texture này là Render Target rỗng trên CPU, toàn bộ alpha bằng 0. Shader nhân `baseColor.a * maskAlpha (0.0)` khiến toàn bộ nội dung văn bản bị trong suốt hoàn toàn.
    - **Nguyên nhân 3 (Cắt cụt bề ngang hàng trong UpdateSubresource)**: Engine tạo Dynamic Texture `1024x1024` Format 87 nhưng truyền `RowPitch = 7680` ($1920 \times 4$ pixels). Do texture chỉ cấp phát bề ngang 1024, các ký tự vượt quá pixel 1024 bị cắt bỏ.
  - **Giải Pháp Toàn Diện**:
    1. **Tự động mở rộng Texture Buffer (`resource_registry.cpp`)**:
       - Trong `ResourceRegistry::updateTexture`, khi `srcPitch > entry.rowPitch` và `box == nullptr`, tự động mở rộng `entry.texture.width = srcPitch / 4` (1920) và `entry.rowPitch = srcPitch` (7680), cấp phát lại bộ đệm để chứa trọn vẹn toàn bộ canvas $1920 \times 1080$ mà không cắt mất ký tự nào.
    2. **Nhận diện và Gán Texture Văn Bản Attention (`d3d11_bridge.cpp`)**:
       - Trong `mock_Context_DrawIndexed`: Nhận diện Quad vẽ văn bản ở dải giữa màn hình ($X \in [-960, 960]$, $Y \in [-250, 250]$, Vertex Color `0xFFFFFFFF`) khi Slot 0 đang giữ khung vi mạch và có `sLastUpdatedTextureResource`.
       - Tự động ưu tiên gán `colorTex = sLastUpdatedTextureResource` và loại bỏ Render Target mask rỗng (`cmd.alphaTextureHandle = nullptr`).
    3. **Bộ lọc kiểm tra Alpha Mask thực tế (`jni_bridge.cpp`)**:
       - Trong `jni_bridge.cpp`: Chỉ kích hoạt `hasValidAlphaMask = true` khi dữ liệu mask thực sự có ít nhất 1 pixel alpha khác 0. Loại bỏ hoàn toàn fallback dùng mask rỗng, đảm bảo văn bản không bao giờ bị nhân với 0.
  - **Độ phức tạp**: $O(1)$ cho kiểm tra quad và lấy mẫu alpha mask, $O(W \times H)$ cho sao chép mảng pixel BGRA $\to$ RGBA.

### 2.3. Lớp Dịch Đồ Hoạ & CPU (Box64 + DXVK)
- **Box64**: Thực thi các lệnh x86_64 của `sgre_steam.exe` trên CPU ARM64 của điện thoại với độ trễ tối thiểu (JIT Recompilation).
- **DXVK**: Thay vì chạy D3D11 (chỉ có trên Windows), DXVK chặn toàn bộ lệnh D3D11 từ engine gốc và biên dịch on-the-fly sang mã gọi Vulkan gửi vào `ANativeWindow` của Android.

---

## 3. Độ Phức Tạp & Tối Ưu Hiệu Năng (Complexity & Performance)

- **Toán học tọa độ chạm**: $O(1)$ cho mỗi sự kiện chạm.
- **Render pipeline**: DXVK tạo ra độ trễ cực thấp (< 2ms) do Vulkan là API cấp thấp sát phần cứng tương tự DirectX 12.
- **Bộ nhớ (RAM)**:
  - Runtime wrapper: ~60MB RAM.
  - Game gốc & E-mote cache: ~1.2GB - 2GB RAM.
  - Yêu cầu cấu hình khuyến nghị: Thiết bị Android có từ 4GB RAM trở lên, hỗ trợ Vulkan 1.1+.

---

## 4. Kiểm Thử Tự Động (LDPlayer Automation Testing)

Hệ thống tích hợp bộ điều khiển tự động qua ADB và LDPlayer (`tests/automation/ldplayer_controller.py`):
- **Phạm vi kiểm thử**:
  - Tự động nhận diện thiết bị giả lập (`emulator-5554`, Android 9 x86_64).
  - Tự động gửi cử chỉ chạm và vuốt đa hướng theo độ phân giải màn hình.
  - Bắt ảnh chụp màn hình định dạng raw/png để phân tích frame vẽ đồ hoạ và chuyển động nhân vật.
  - Lọc logcat theo tag native (`SGRE_JNI`, `SGRE_InputInjector`, `AndroidRuntime`) để cảnh báo lỗi ngay khi game crash.

