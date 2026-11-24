#include "input_injector.h"
#include "win32_bridge.h"
#include "dinput_bridge.h"
#include "xinput_bridge.h"
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

void InputInjector::sendWin32Mouse(uint32_t msg, int32_t x, int32_t y, int16_t wheelDelta) {
    LOGI("Dispatched Win32 Mouse Msg: 0x%X at (%d, %d)", msg, x, y);
    Win32Bridge_SetCursorPos(x, y);

    if (msg == WM_LBUTTONDOWN) {
        Win32Bridge_SetKeyState(0x01, true); // VK_LBUTTON
    } else if (msg == WM_LBUTTONUP) {
        Win32Bridge_SetKeyState(0x01, false);
    } else if (msg == WM_RBUTTONDOWN) {
        Win32Bridge_SetKeyState(0x02, true); // VK_RBUTTON
    } else if (msg == WM_RBUTTONUP) {
        Win32Bridge_SetKeyState(0x02, false);
    }

    int64_t lParam = (static_cast<int64_t>(y) << 16) | (x & 0xFFFF);
    uint64_t wParam = 0;
    if (msg == WM_LBUTTONDOWN) {
        wParam = 1;
    } else if (msg == WM_RBUTTONDOWN) {
        wParam = 2;
    } else if (msg == WM_MOUSEWHEEL) {
        wParam = (static_cast<uint64_t>(static_cast<uint16_t>(wheelDelta)) << 16);
    }
    Win32Bridge_PostMessage(msg, wParam, lParam);
}

void InputInjector::sendWin32Key(uint32_t virtualKey, bool isDown) {
    LOGI("Dispatched Win32 Key: 0x%X, isDown: %d", virtualKey, isDown ? 1 : 0);
    DInputBridge_PostKeyEvent(virtualKey, isDown);
    uint32_t msg = isDown ? 0x0100 : 0x0101; // WM_KEYDOWN / WM_KEYUP
    Win32Bridge_PostMessage(msg, virtualKey, 0);
}

void InputInjector::injectQuickPulseKey(uint32_t virtualKey) {
    sendWin32Key(virtualKey, true);
    sendWin32Key(virtualKey, false);
}

void InputInjector::injectKeyEvent(int32_t keyCode, bool isDown) {
    XInputBridge_UpdateButton(keyCode, isDown);
    uint32_t vk = 0;
    switch (keyCode) {
        case 66:  // KEYCODE_ENTER
        case 160: // KEYCODE_NUMPAD_ENTER
        case 96:  // KEYCODE_BUTTON_A
        case 23:  // KEYCODE_DPAD_CENTER
            vk = 0x0D; // VK_RETURN (A button)
            break;
        case 62:  // KEYCODE_SPACE
            vk = 0x20; // VK_SPACE (A button)
            break;
        case 4:   // KEYCODE_BACK
        case 97:  // KEYCODE_BUTTON_B
        case 67:  // KEYCODE_DEL (Backspace)
            vk = 0x08; // VK_BACK (B button / Back / Cancel)
            break;
        case 111: // KEYCODE_ESCAPE
            vk = 0x1B; // VK_ESCAPE (System Menu / Exit Dialog)
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
        case 99:  // KEYCODE_BUTTON_X
            vk = 0x2E; // VK_DELETE (X button)
            break;
        case 100: // KEYCODE_BUTTON_Y
        case 109: // KEYCODE_BUTTON_SELECT
            vk = 0x2D; // VK_INSERT (Y button)
            break;
        case 102: // KEYCODE_BUTTON_L1
        case 92:  // KEYCODE_PAGE_UP
            vk = 0x51; // 'Q' (L1 button)
            break;
        case 103: // KEYCODE_BUTTON_R1
        case 93:  // KEYCODE_PAGE_DOWN
            vk = 0x45; // 'E' (R1 button)
            break;
        case 104: // KEYCODE_BUTTON_L2
            vk = 0x5A; // 'Z' (L2 button)
            break;
        case 105: // KEYCODE_BUTTON_R2
            vk = 0x43; // 'C' (R2 button)
            break;
        case 108: // KEYCODE_BUTTON_START
        case 82:  // KEYCODE_MENU
            vk = 0x31; // '1' (START button)
            break;
        case 113: // KEYCODE_CTRL_LEFT
        case 114: // KEYCODE_CTRL_RIGHT
            vk = 0x11; // VK_CONTROL
            break;
        case 131: // KEYCODE_F1
        case 132: // KEYCODE_F2
        case 133: // KEYCODE_F3
        case 134: // KEYCODE_F4
        case 135: // KEYCODE_F5
        case 136: // KEYCODE_F6
        case 137: // KEYCODE_F7
        case 138: // KEYCODE_F8
        case 139: // KEYCODE_F9
        case 140: // KEYCODE_F10
        case 141: // KEYCODE_F11
        case 142: // KEYCODE_F12
            vk = static_cast<uint32_t>(0x70 + (keyCode - 131)); // VK_F1 .. VK_F12
            break;
        default:
            if (keyCode >= 29 && keyCode <= 54) { // A-Z
                vk = static_cast<uint32_t>(0x41 + (keyCode - 29));
            } else if (keyCode >= 7 && keyCode <= 16) { // 0-9
                vk = static_cast<uint32_t>(0x30 + (keyCode - 7));
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
            DInputBridge_PostTouchAction(0, targetX, targetY);
            break;

        case GameTouchAction::TouchMove:
            sendWin32Mouse(WM_MOUSEMOVE, targetX, targetY);
            DInputBridge_PostTouchAction(1, targetX, targetY);
            break;

        case GameTouchAction::TouchUp:
            sendWin32Mouse(WM_LBUTTONUP, targetX, targetY);
            sendWin32Key(VK_CONTROL, false);
            DInputBridge_PostTouchAction(2, targetX, targetY);
            break;

        case GameTouchAction::RightClick:
            sendWin32Mouse(WM_MOUSEMOVE, targetX, targetY);
            sendWin32Mouse(WM_RBUTTONDOWN, targetX, targetY);
            sendWin32Mouse(WM_RBUTTONUP, targetX, targetY);
            DInputBridge_PostTouchAction(7, targetX, targetY);
            break;

        case GameTouchAction::SwipeUp:
            // Vuốt lên: Mở lịch sử backlog thoại (Wheel Up + VK_UP)
            DInputBridge_PostMouseWheel(120);
            sendWin32Mouse(WM_MOUSEWHEEL, targetX, targetY, 120);
            DInputBridge_PostKeyEvent(0x26, true);
            DInputBridge_PostKeyEvent(0x26, false);
            break;

        case GameTouchAction::SwipeDown:
            // Vuốt xuống: Đóng hoặc cuộn xuống (Wheel Down + VK_DOWN)
            DInputBridge_PostMouseWheel(-120);
            sendWin32Mouse(WM_MOUSEWHEEL, targetX, targetY, -120);
            DInputBridge_PostKeyEvent(0x28, true);
            DInputBridge_PostKeyEvent(0x28, false);
            break;

        case GameTouchAction::SwipeRight:
            // Vuốt phải: Ẩn hộp thoại (tương đương Right Click)
            sendWin32Mouse(WM_MOUSEMOVE, targetX, targetY);
            sendWin32Mouse(WM_RBUTTONDOWN, targetX, targetY);
            sendWin32Mouse(WM_RBUTTONUP, targetX, targetY);
            DInputBridge_PostTouchAction(7, targetX, targetY);
            LOGI("Triggered Hide Dialogue Box via SwipeRight (Right Click)");
            break;

        case GameTouchAction::LongPress:
            // Giữ ngón tay: Kích hoạt tua nhanh (Skip mode)
            sendWin32Key(VK_CONTROL, true);
            LOGI("Triggered Fast Forward Skip via LongPress (VK_CONTROL)");
            break;
    }
}
