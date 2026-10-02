# SGRB-port: STEINS;GATE REBOOT Android Native Wrapper

Dự án port **STEINS;GATE REBOOT** sang hệ điều hành **Android (ARM64)** sử dụng kiến trúc Binary Wrapper Native:
- **100% Giữ nguyên chuyển động & logic gốc**: Chạy trực tiếp engine C++ `sgre_steam.exe` và hệ thống E-mote của M2 thông qua lớp biên dịch động x86_64 sang ARM64 (Box64).
- **Đồ họa D3D11 to Vulkan**: Dịch lệnh dựng hình DirectX 11 sang Vulkan qua DXVK để tận dụng tối đa GPU trên Android (Adreno / Mali).
- **Hỗ trợ cảm ứng toàn diện (Native Touch Bridge)**: Ánh xạ cử chỉ chạm, vuốt, giữ thành các thao tác điều khiển mượt mà trên điện thoại.

---

## 1. Cấu Trúc Dự Án (Project Structure)

```
SGRB-port/
├── app/
│   ├── src/main/
│   │   ├── java/com/makurisu/sgrb/
│   │   │   ├── MainActivity.java           # Điểm khởi chạy Android Activity
│   │   │   ├── TouchInputHandler.java       # Bộ xử lý cử chỉ cảm ứng (Tap, Swipe, Long press)
│   │   │   └── GameView.java               # Surface hiển thị hình ảnh từ Vulkan
│   │   ├── cpp/
│   │   │   ├── CMakeLists.txt              # Build script NDK C++
│   │   │   ├── jni_bridge.cpp              # Cầu nối JNI giữa Java và Core Native
│   │   │   ├── input_injector.cpp          # Bơm sự kiện chuột/phím Win32 vào game loop
│   │   │   └── steam_hook.cpp              # Giả lập Steam API (steam_api64.dll mock)
│   │   └── AndroidManifest.xml
│   └── build.gradle
├── CODEBASE-DOCS/
│   └── ANDROID_PORT_ARCHITECTURE.md        # Tài liệu chi tiết kiến trúc và luồng xử lý
├── .github/workflows/
│   └── build-apk.yml                       # CI/CD tự động build APK trên GitHub
└── README.md
```

---

## 2. Thao Tác Cảm Ứng (Touch Controls)

| Cử chỉ (Gesture) | Chức năng trong game |
| :--- | :--- |
| **Chạm 1 ngón (Single Tap)** | Chuyển câu thoại kế tiếp, bấm nút menu |
| **Vuốt từ dưới lên (Swipe Up)** | Mở lịch sử thoại (Backlog) |
| **Vuốt từ trên xuống (Swipe Down)** | Mở menu cài đặt & Save/Load |
| **Giữ 1 hoặc 2 ngón (Long Press)** | Bật chế độ tua nhanh (Skip mode) |
| **Vuốt từ trái sang phải (Swipe Right)** | Ẩn hộp thoại |

---

## 3. Quản Lý Dữ Liệu Game (Game Data Storage)

Do dung lượng game gốc lớn (>5GB), dữ liệu game được nạp trực tiếp từ bộ nhớ ngoài:
`/sdcard/Android/data/com.makurisu.sgrb/files/data/`
Bao gồm:
- Toàn bộ file kịch bản `.psb`
- Dữ liệu texture E-mote
- Nhạc nền, âm thanh và video cutscene (`steinsgate_live_en.mp4`)
