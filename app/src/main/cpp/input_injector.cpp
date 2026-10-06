#include "input_injector.h"
#include "win32_bridge.h"
#include <android/log.h>
#include <algorithm>

#define LOG_TAG "SGRE_InputInjector"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
    constexpr int32_t BASE_RES_X = 1920;
    constexpr int32_t BASE_RES_Y = 1080;

    // Hằng số Win32 message
    constexpr uint32_t WM_LBUTTONDOWN = 0x0201;
    constexpr uint32_t WM_LBUTTONUP = 0x0202;
    constexpr uint32_t WM_MOUSEMOVE = 0x0200;
    constexpr uint32_t WM_RBUTTONDOWN = 0x0204;
    constexpr uint32_t WM_RBUTTONUP = 0x0205;
    constexpr uint32_t WM_MOUSEWHEEL = 0x020A;
    constexpr uint32_t VK_CONTROL = 0x11;
}

void InputInjector::sendWin32Mouse(uint32_t msg, int32_t x, int32_t y) {
    LOGI("Dispatched Win32 Mouse Msg: 0x%X at (%d, %d)", msg, x, y);
    Win32Bridge_SetCursorPos(x, y);

    if (msg == WM_LBUTTONDOWN) {
        Win32Bridge_SetKeyState(0x01, true); // VK_LBUTTON
        Win32Bridge_SetKeyState(0x0D, true); // VK_RETURN
        Win32Bridge_SetKeyState(0x20, true); // VK_SPACE
    } else if (msg == WM_LBUTTONUP) {
        Win32Bridge_SetKeyState(0x01, false);
        Win32Bridge_SetKeyState(0x0D, false);
        Win32Bridge_SetKeyState(0x20, false);
    } else if (msg == WM_RBUTTONDOWN) {
        Win32Bridge_SetKeyState(0x02, true); // VK_RBUTTON
    } else if (msg == WM_RBUTTONUP) {
        Win32Bridge_SetKeyState(0x02, false);
    }

    int64_t lParam = (static_cast<int64_t>(y) << 16) | (x & 0xFFFF);
    uint64_t wParam = (msg == WM_LBUTTONDOWN) ? 1 : 0;
    Win32Bridge_PostMessage(msg, wParam, lParam);
}

void InputInjector::sendWin32Key(uint32_t virtualKey, bool isDown) {
    LOGI("Dispatched Win32 Key: 0x%X, isDown: %d", virtualKey, isDown ? 1 : 0);
    Win32Bridge_SetKeyState(static_cast<uint8_t>(virtualKey), isDown);
    uint32_t msg = isDown ? 0x0100 : 0x0101; // WM_KEYDOWN / WM_KEYUP
    Win32Bridge_PostMessage(msg, virtualKey, 0);
}

void InputInjector::injectKeyEvent(int32_t keyCode, bool isDown) {
    uint32_t vk = 0;
    switch (keyCode) {
        case 66:  // KEYCODE_ENTER
        case 160: // KEYCODE_NUMPAD_ENTER
        case 96:  // KEYCODE_BUTTON_A
            vk = 0x0D; // VK_RETURN
            break;
        case 62:  // KEYCODE_SPACE
            vk = 0x20; // VK_SPACE
            break;
        case 111: // KEYCODE_ESCAPE
        case 4:   // KEYCODE_BACK
        case 97:  // KEYCODE_BUTTON_B
            vk = 0x1B; // VK_ESCAPE
            break;
        case 19:  // KEYCODE_DPAD_UP
            vk = 0x26; // VK_UP
            break;
        case 20:  // KEYCODE_DPAD_DOWN
            vk = 0x28; // VK_DOWN
            break;
        case 21:  // KEYCODE_DPAD_LEFT
            vk = 0x25; // VK_LEFT
            break;
        case 22:  // KEYCODE_DPAD_RIGHT
            vk = 0x27; // VK_RIGHT
            break;
        case 54:  // KEYCODE_Z
        case 99:  // KEYCODE_BUTTON_X
            vk = 0x5A; // 'Z'
            break;
        case 52:  // KEYCODE_X
        case 100: // KEYCODE_BUTTON_Y
            vk = 0x58; // 'X'
            break;
        case 31:  // KEYCODE_C
            vk = 0x43; // 'C'
            break;
        case 113: // KEYCODE_CTRL_LEFT
        case 114: // KEYCODE_CTRL_RIGHT
            vk = 0x11; // VK_CONTROL
            break;
        default:
            if (keyCode >= 29 && keyCode <= 54) { // A-Z
                vk = static_cast<uint32_t>(0x41 + (keyCode - 29));
            }
            break;
    }

    if (vk != 0) {
        sendWin32Key(vk, isDown);
    }
}

void InputInjector::injectTouchEvent(GameTouchAction action, float normalizedX, float normalizedY) {
    int32_t targetX = static_cast<int32_t>(std::clamp(normalizedX, 0.0f, 1.0f) * BASE_RES_X);
    int32_t targetY = static_cast<int32_t>(std::clamp(normalizedY, 0.0f, 1.0f) * BASE_RES_Y);

    switch (action) {
        case GameTouchAction::TouchDown:
            sendWin32Mouse(WM_MOUSEMOVE, targetX, targetY);
            sendWin32Mouse(WM_LBUTTONDOWN, targetX, targetY);
            break;

        case GameTouchAction::TouchMove:
            sendWin32Mouse(WM_MOUSEMOVE, targetX, targetY);
            break;

        case GameTouchAction::TouchUp:
            sendWin32Mouse(WM_LBUTTONUP, targetX, targetY);
            sendWin32Key(VK_CONTROL, false);
            break;

        case GameTouchAction::SwipeUp:
            // Vuốt lên: Mở lịch sử backlog thoại
            sendWin32Mouse(WM_MOUSEWHEEL, targetX, targetY);
            break;

        case GameTouchAction::SwipeDown:
            // Vuốt xuống: Mở/đóng System Menu (chuột phải)
            sendWin32Mouse(WM_RBUTTONDOWN, targetX, targetY);
            sendWin32Mouse(WM_RBUTTONUP, targetX, targetY);
            break;

        case GameTouchAction::SwipeRight:
            // Vuốt phải: Ẩn hộp thoại
            LOGI("Triggered Hide Dialogue Box");
            break;

        case GameTouchAction::LongPress:
            // Giữ ngón tay: Kích hoạt tua nhanh (Skip mode)
            sendWin32Key(VK_CONTROL, true);
            break;
    }
}
