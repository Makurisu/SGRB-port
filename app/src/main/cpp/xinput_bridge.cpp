#include "xinput_bridge.h"
#include "input_injector.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

constexpr float kAnalogStickDeadzone = 0.35f;
constexpr float kTriggerDigitalThreshold = 0.50f;
constexpr float kMaxThumbAxisValue = 32767.0f;
constexpr float kMaxTriggerByteValue = 255.0f;

// Mã phím Win32 dùng để đồng bộ song song sang DirectInput / Win32 Message Queue
constexpr uint32_t kVkUp    = 0x26;
constexpr uint32_t kVkDown  = 0x28;
constexpr uint32_t kVkLeft  = 0x25;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkKeyZ  = 0x5A; // L2
constexpr uint32_t kVkKeyC  = 0x43; // R2

struct ControllerState {
    std::mutex mutex;
    bool isConnected = false;
    uint32_t packetNumber = 1;
    uint16_t digitalButtons = 0;
    uint16_t axisButtons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;

    bool prevNavUp = false;
    bool prevNavDown = false;
    bool prevNavLeft = false;
    bool prevNavRight = false;
    bool prevL2Pressed = false;
    bool prevR2Pressed = false;
};

static ControllerState sController;

static int16_t quantizeAxis(float value, bool invertAxis) {
    float clamped = std::clamp(value, -1.0f, 1.0f);
    if (std::fabs(clamped) < 0.12f) {
        return 0;
    }
    if (invertAxis) {
        clamped = -clamped;
    }
    int32_t scaled = static_cast<int32_t>(std::lround(clamped * kMaxThumbAxisValue));
    return static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
}

static uint8_t quantizeTrigger(float value) {
    float clamped = std::clamp(value, 0.0f, 1.0f);
    int32_t scaled = static_cast<int32_t>(std::lround(clamped * kMaxTriggerByteValue));
    return static_cast<uint8_t>(std::clamp(scaled, 0, 255));
}

static uint16_t mapAndroidKeyToXInputMask(int32_t keyCode) {
    switch (keyCode) {
        case 19:  return XINPUT_BTN_DPAD_UP;        // KEYCODE_DPAD_UP
        case 20:  return XINPUT_BTN_DPAD_DOWN;      // KEYCODE_DPAD_DOWN
        case 21:  return XINPUT_BTN_DPAD_LEFT;      // KEYCODE_DPAD_LEFT
        case 22:  return XINPUT_BTN_DPAD_RIGHT;     // KEYCODE_DPAD_RIGHT
        case 96:  return XINPUT_BTN_A;              // KEYCODE_BUTTON_A
        case 97:  return XINPUT_BTN_B;              // KEYCODE_BUTTON_B
        case 99:  return XINPUT_BTN_X;              // KEYCODE_BUTTON_X
        case 100: return XINPUT_BTN_Y;              // KEYCODE_BUTTON_Y
        case 102: return XINPUT_BTN_LEFT_SHOULDER;  // KEYCODE_BUTTON_L1
        case 103: return XINPUT_BTN_RIGHT_SHOULDER; // KEYCODE_BUTTON_R1
        case 106: return XINPUT_BTN_LEFT_THUMB;     // KEYCODE_BUTTON_THUMBL
        case 107: return XINPUT_BTN_RIGHT_THUMB;    // KEYCODE_BUTTON_THUMBR
        case 108: return XINPUT_BTN_START;          // KEYCODE_BUTTON_START
        case 109: return XINPUT_BTN_BACK;           // KEYCODE_BUTTON_SELECT
        default:  return 0;
    }
}

} // namespace

extern "C" {

void XInputBridge_SetConnected(bool connected) {
    std::lock_guard<std::mutex> lock(sController.mutex);
    if (sController.isConnected != connected) {
        sController.isConnected = connected;
        sController.packetNumber++;
    }
}

void XInputBridge_UpdateButton(int32_t androidKeyCode, bool isDown) {
    uint16_t mask = mapAndroidKeyToXInputMask(androidKeyCode);
    if (mask == 0) return;

    std::lock_guard<std::mutex> lock(sController.mutex);
    // Chỉ đánh dấu tay cầm thực sự kết nối khi nhận nút bấm chuẩn Gamepad (>= 96)
    if (androidKeyCode >= 96 && androidKeyCode <= 110) {
        sController.isConnected = true;
    }
    uint16_t before = sController.digitalButtons;
    if (isDown) {
        sController.digitalButtons |= mask;
    } else {
        sController.digitalButtons &= static_cast<uint16_t>(~mask);
    }
    if (sController.digitalButtons != before) {
        sController.packetNumber++;
    }
}

void XInputBridge_UpdateAxes(
    float leftX,
    float leftY,
    float rightX,
    float rightY,
    float leftTrigger,
    float rightTrigger,
    float hatX,
    float hatY
) {
    bool sendUp = false, relUp = false;
    bool sendDown = false, relDown = false;
    bool sendLeft = false, relLeft = false;
    bool sendRight = false, relRight = false;
    bool sendL2 = false, relL2 = false;
    bool sendR2 = false, relR2 = false;

    {
        std::lock_guard<std::mutex> lock(sController.mutex);
        sController.isConnected = true;

        // Trong Android MotionEvent trục Y hướng xuống (+1), còn XInput sThumbLY hướng lên (+32767)
        int16_t newLX = quantizeAxis(leftX, false);
        int16_t newLY = quantizeAxis(leftY, true);
        int16_t newRX = quantizeAxis(rightX, false);
        int16_t newRY = quantizeAxis(rightY, true);
        uint8_t newLT = quantizeTrigger(leftTrigger);
        uint8_t newRT = quantizeTrigger(rightTrigger);

        uint16_t hatButtons = 0;
        if (hatY < -kAnalogStickDeadzone) hatButtons |= XINPUT_BTN_DPAD_UP;
        if (hatY > kAnalogStickDeadzone)  hatButtons |= XINPUT_BTN_DPAD_DOWN;
        if (hatX < -kAnalogStickDeadzone) hatButtons |= XINPUT_BTN_DPAD_LEFT;
        if (hatX > kAnalogStickDeadzone)  hatButtons |= XINPUT_BTN_DPAD_RIGHT;

        bool navUp    = (leftY < -kAnalogStickDeadzone) || (hatY < -kAnalogStickDeadzone);
        bool navDown  = (leftY > kAnalogStickDeadzone)  || (hatY > kAnalogStickDeadzone);
        bool navLeft  = (leftX < -kAnalogStickDeadzone) || (hatX < -kAnalogStickDeadzone);
        bool navRight = (leftX > kAnalogStickDeadzone)  || (hatX > kAnalogStickDeadzone);
        bool l2Down   = (leftTrigger >= kTriggerDigitalThreshold);
        bool r2Down   = (rightTrigger >= kTriggerDigitalThreshold);

        if (navUp != sController.prevNavUp) {
            sendUp = navUp; relUp = !navUp; sController.prevNavUp = navUp;
        }
        if (navDown != sController.prevNavDown) {
            sendDown = navDown; relDown = !navDown; sController.prevNavDown = navDown;
        }
        if (navLeft != sController.prevNavLeft) {
            sendLeft = navLeft; relLeft = !navLeft; sController.prevNavLeft = navLeft;
        }
        if (navRight != sController.prevNavRight) {
            sendRight = navRight; relRight = !navRight; sController.prevNavRight = navRight;
        }
        if (l2Down != sController.prevL2Pressed) {
            sendL2 = l2Down; relL2 = !l2Down; sController.prevL2Pressed = l2Down;
        }
        if (r2Down != sController.prevR2Pressed) {
            sendR2 = r2Down; relR2 = !r2Down; sController.prevR2Pressed = r2Down;
        }

        if (newLX != sController.thumbLX || newLY != sController.thumbLY ||
            newRX != sController.thumbRX || newRY != sController.thumbRY ||
            newLT != sController.leftTrigger || newRT != sController.rightTrigger ||
            hatButtons != sController.axisButtons) {
            sController.thumbLX = newLX;
            sController.thumbLY = newLY;
            sController.thumbRX = newRX;
            sController.thumbRY = newRY;
            sController.leftTrigger = newLT;
            sController.rightTrigger = newRT;
            sController.axisButtons = hatButtons;
            sController.packetNumber++;
        }
    }

    // Đồng bộ cạnh chuyển đổi sang DirectInput / Win32 ngoài phạm vi mutex tránh khóa lồng nhau
    if (sendUp || relUp)       InputInjector::sendWin32Key(kVkUp, sendUp);
    if (sendDown || relDown)   InputInjector::sendWin32Key(kVkDown, sendDown);
    if (sendLeft || relLeft)   InputInjector::sendWin32Key(kVkLeft, sendLeft);
    if (sendRight || relRight) InputInjector::sendWin32Key(kVkRight, sendRight);
    if (sendL2 || relL2)       InputInjector::sendWin32Key(kVkKeyZ, sendL2);
    if (sendR2 || relR2)       InputInjector::sendWin32Key(kVkKeyC, sendR2);
}

uint32_t WINAPI XInputBridge_GetState(uint32_t dwUserIndex, void* pState) {
    if (pState != nullptr) {
        std::memset(pState, 0, sizeof(WinXInputState));
    }
    if (dwUserIndex != 0 || pState == nullptr) {
        return XINPUT_RESULT_DEVICE_NOT_CONNECTED;
    }

    std::lock_guard<std::mutex> lock(sController.mutex);
    if (!sController.isConnected) {
        return XINPUT_RESULT_DEVICE_NOT_CONNECTED;
    }

    auto* out = reinterpret_cast<WinXInputState*>(pState);
    out->dwPacketNumber = sController.packetNumber;
    out->gamepad.wButtons = static_cast<uint16_t>(sController.digitalButtons | sController.axisButtons);
    out->gamepad.bLeftTrigger = sController.leftTrigger;
    out->gamepad.bRightTrigger = sController.rightTrigger;
    out->gamepad.sThumbLX = sController.thumbLX;
    out->gamepad.sThumbLY = sController.thumbLY;
    out->gamepad.sThumbRX = sController.thumbRX;
    out->gamepad.sThumbRY = sController.thumbRY;
    return XINPUT_RESULT_SUCCESS;
}

uint32_t WINAPI XInputBridge_SetState(uint32_t dwUserIndex, void* /* pVibration */) {
    if (dwUserIndex != 0) {
        return XINPUT_RESULT_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard<std::mutex> lock(sController.mutex);
    return sController.isConnected ? XINPUT_RESULT_SUCCESS : XINPUT_RESULT_DEVICE_NOT_CONNECTED;
}

} // extern "C"
