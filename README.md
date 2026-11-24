# SGRB-port — STEINS;GATE REBOOT Android Compatibility Wrapper

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/Makurisu/SGRB-port)](https://github.com/Makurisu/SGRB-port/releases)
[![Build Android APK](https://github.com/Makurisu/SGRB-port/actions/workflows/build-apk.yml/badge.svg)](https://github.com/Makurisu/SGRB-port/actions/workflows/build-apk.yml)
![Android API](https://img.shields.io/badge/Android-8.0%2B%20(API%2026%2B)-3DDC84)
![ABI](https://img.shields.io/badge/ABI-x86__64%20%7C%20arm64--v8a-orange)

[English](#english) | [Tiếng Việt](#tieng-viet)

---

<a id="english"></a>
## English

### About

**SGRB-port** is an open-source Android native compatibility runtime for **STEINS;GATE REBOOT** (`com.makurisu.sgrb`).  
It loads the Windows x86-64 game binary directly into memory and bridges its subsystems (Win32 VFS, Direct3D 11 to OpenGL ES 3, XAudio2/AAudio, WebM/MediaFoundation, Steamworks stub, and touch/controller input) natively on Android without bundling proprietary game files.

### Current Status & Requirements

- **Supported Runtime**:
  - `x86_64Only` flavor: Runs natively on `x86_64` Android devices/emulators AND on `arm64-v8a` devices equipped with system binary translation (`libndk_translation` / `houdini`).
  - `arm64Box64` / `universal` flavors: Includes the dynamic `Arm64Box64Bridge` (`ms_abi` <-> `AAPCS64` thunking) ready to load `libbox64.so` from `<dataDir>/box64/libbox64.so`.
- **Graphics, Audio & Mobile UX**:
  - 16:9 Letterbox/Pillarbox aspect-ratio preservation (with runtime Full-Screen Stretch toggle), 30/60 FPS limiter, specialized GLSL binary shader cache, SIMD SSE2/NEON WebM video decoding, central single-stream `XAudio2MasterMixer`, full XInput/DirectInput Bluetooth/USB controller support, collapsible Quick Overlay HUD (Phone Trigger, Auto, Skip, Quick Save, System Menu, FPS/RAM monitor), and ZIP Save Backup/Restore.
- **Android Version**: Android 8.0+ (API 26 minimum, target SDK 34).
- **Game Files Required**: You must provide files from your own legitimate PC copy of *STEINS;GATE REBOOT*.

### Installation

1. Download `STEINS.GATE.REBOOT-v1.0.0.apk` from the [Releases](https://github.com/Makurisu/SGRB-port/releases) page.
2. Install the APK on your Android `x86_64` device or emulator (enable *Install unknown apps* if prompted, or install via ADB):
   ```bash
   adb install -r -g STEINS.GATE.REBOOT-v1.0.0.apk
   ```
3. Copy your PC game files (`sgre_steam.exe` and all data folders/archives) to your device at **one** of the following paths:
   - **Recommended**: `/sdcard/sgrb_game_data/` (checked first when `sgre_steam.exe` is present)
   - **Alternative**: `/sdcard/Android/data/com.makurisu.sgrb/files/data/`
   ```bash
   adb push <PC_GAME_FOLDER>/. /sdcard/sgrb_game_data/
   ```
4. Grant Storage / All Files Access permission if requested and launch **STEINS;GATE REBOOT**.

### Touch & Key Controls

| Input | Action |
| :--- | :--- |
| **Single Tap** | Advance dialogue / Select menu item |
| **Two-Finger Tap** | Right-click (Cancel / Back / Hide dialogue) |
| **Swipe Right** | Hide or toggle dialogue box |
| **Swipe Up / Down** | Open Backlog / Scroll |
| **Long Press (450ms+)** | Hold Skip (Fast-Forward) |
| **Android Back Button** | Cancel / Close menu (`B` button) |

### Build from Source

Prerequisites: **JDK 17**, **Android SDK (API 34)**, **Android NDK (r26b+)**, **CMake 3.22.1+**, **Python 3**.

```bash
# Run Python & JVM unit tests
python -m unittest discover -s tests -t tests
./gradlew testX86_64OnlyDebugUnitTest

# Build ABI-specific or Universal APKs
./gradlew assembleX86_64OnlyDebug assembleArm64Box64Debug assembleUniversalDebug
```

### License & Third-Party Notices

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)** — see [LICENSE](LICENSE) for full terms.

- `app/src/main/cpp/wmadeci.c`, `wmadeci.h`, `codeccontext.h`: Fixed-point WMA decoder derived from Rockbox (Copyright (C) 2005 Dave Chapman, GNU GPL).
- `app/src/main/cpp/third_party/bcdec/bcdec.h`: BC1–BC7 block texture decoder by Sergii "iOrange" Kudlai (MIT License / The Unlicense).

### Legal Notice

This repository contains **only** the open-source compatibility wrapper code. It does not include copyrighted binaries, scripts, textures, audio, or video assets from *STEINS;GATE REBOOT*. *STEINS;GATE* is a trademark of MAGES. / Chiyomaru Studio.

---

<a id="tieng-viet"></a>
## Tiếng Việt

### Giới Thiệu

**SGRB-port** là dự án mã nguồn mở xây dựng lớp tương thích Android cho **STEINS;GATE REBOOT** (`com.makurisu.sgrb`).  
Ứng dụng nạp trực tiếp tệp thực thi Windows x86-64 vào bộ nhớ và ánh xạ các hệ thống con (Win32 VFS, Direct3D 11 sang OpenGL ES 3, XAudio2/AAudio, WebM/MediaFoundation, Steamworks stub và điều khiển cảm ứng/tay cầm) sang API gốc của Android.

### Yêu Cầu & Trạng Thái Hỗ Trợ

- **Các gói cài đặt (Product Flavors)**:
  - Bản `x86_64Only`: Chạy trực tiếp tốc độ gốc trên thiết bị/giả lập `x86_64`, đồng thời tự động kích hoạt trình dịch nhị phân hệ thống (`libndk_translation` / `houdini`) trên nhiều điện thoại `arm64-v8a` Android 11+.
  - Bản `arm64Box64` / `universal`: Tích hợp cầu nối động `Arm64Box64Bridge` chuyển tiếp thanh ghi `ms_abi` <-> `AAPCS64`, hỗ trợ nạp `libbox64.so` từ `<thư_mục_game>/box64/libbox64.so`.
- **Tính năng & Tối ưu di động**:
  - Giữ chuẩn khung hình 16:9 (kèm tùy chọn kéo giãn toàn màn hình), khóa 30/60 FPS tiết kiệm pin, bộ nhớ đệm nhị phân cho 4 họ shader GLSL, giải mã video WebM tăng tốc SIMD SSE2/NEON, bộ trộn âm thanh trung tâm `XAudio2MasterMixer`, hỗ trợ đầy đủ tay cầm XInput/Bluetooth, thanh phím tắt nổi thu gọn (Mở điện thoại, Tự động, Tua nhanh, Lưu nhanh, Menu, đo FPS/RAM) và Sao lưu/Khôi phục Save `.zip` an toàn.
- **Hệ điều hành**: Android 8.0 trở lên (Min SDK 26, Target SDK 34).
- **Dữ liệu game**: Không đi kèm trong APK; người dùng cần tự sao chép từ bản PC hợp pháp của mình.

### Hướng Dẫn Cài Đặt

1. Tải tệp `STEINS.GATE.REBOOT-v1.0.0.apk` tại trang [Releases](https://github.com/Makurisu/SGRB-port/releases).
2. Cài đặt APK lên thiết bị hoặc trình giả lập Android `x86_64`:
   ```bash
   adb install -r -g STEINS.GATE.REBOOT-v1.0.0.apk
   ```
3. Chép toàn bộ dữ liệu game PC (`sgre_steam.exe` cùng các thư mục dữ liệu đi kèm) vào **một trong hai** đường dẫn trên thiết bị Android:
   - **Khuyến nghị**: `/sdcard/sgrb_game_data/` (được ưu tiên nạp nếu có `sgre_steam.exe`)
   - **Thư mục dự phòng**: `/sdcard/Android/data/com.makurisu.sgrb/files/data/`
   ```bash
   adb push <THU_MUC_GAME_PC>/. /sdcard/sgrb_game_data/
   ```
4. Cấp quyền truy cập bộ nhớ (nếu hệ thống yêu cầu) và mở ứng dụng **STEINS;GATE REBOOT**.

### Thao Tác Cảm Ứng

| Cử chỉ / Phím | Chức năng |
| :--- | :--- |
| **Chạm 1 ngón** | Qua thoại / Chọn nút trên menu |
| **Chạm 2 ngón** | Chuột phải (Quay lại / Hủy / Ẩn hộp thoại) |
| **Vuốt sang phải** | Ẩn hoặc hiện hộp thoại |
| **Vuốt lên / xuống** | Mở Backlog / Cuộn trang |
| **Chạm giữ (>= 450ms)** | Tua nhanh (Skip) |
| **Nút Back của Android** | Quay lại / Đóng menu (nút `B`) |

### Tự Biên Dịch Từ Mã Nguồn

Yêu cầu: **JDK 17**, **Android SDK (API 34)**, **Android NDK (r26b+)**, **CMake 3.22.1+**, **Python 3**.

```bash
# Chạy toàn bộ unit test Python & JUnit
python -m unittest discover -s tests -t tests
./gradlew testX86_64OnlyDebugUnitTest

# Biên dịch các gói APK theo kiến trúc
./gradlew assembleX86_64OnlyDebug assembleArm64Box64Debug assembleUniversalDebug
```

### Giấy Phép & Mã Nguồn Bên Thứ Ba

Dự án được phát hành theo giấy phép **GNU General Public License v3.0 (GPL-3.0)** — xem chi tiết tại [LICENSE](LICENSE).

- `app/src/main/cpp/wmadeci.c`, `wmadeci.h`, `codeccontext.h`: Bộ giải mã WMA fixed-point từ Rockbox (Copyright (C) 2005 Dave Chapman, GNU GPL).
- `app/src/main/cpp/third_party/bcdec/bcdec.h`: Bộ giải nén texture block BC1–BC7 của Sergii "iOrange" Kudlai (MIT License / The Unlicense).

### Lưu Ý Bản Quyền

Kho mã nguồn này chỉ chứa mã lớp tương thích, hoàn toàn **không** phân phối bất kỳ tài nguyên có bản quyền nào của *STEINS;GATE REBOOT*. Mọi dữ liệu trò chơi thuộc quyền sở hữu của MAGES. / Chiyomaru Studio.
