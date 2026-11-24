#pragma once
#include <cstdint>
#include "win32_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mã trả về chuẩn XInput của Microsoft
constexpr uint32_t XINPUT_RESULT_SUCCESS              = 0;
constexpr uint32_t XINPUT_RESULT_DEVICE_NOT_CONNECTED = 1167;

// Bitmask nút bấm chuẩn XINPUT_GAMEPAD
constexpr uint16_t XINPUT_BTN_DPAD_UP        = 0x0001;
constexpr uint16_t XINPUT_BTN_DPAD_DOWN      = 0x0002;
constexpr uint16_t XINPUT_BTN_DPAD_LEFT      = 0x0004;
constexpr uint16_t XINPUT_BTN_DPAD_RIGHT     = 0x0008;
constexpr uint16_t XINPUT_BTN_START          = 0x0010;
constexpr uint16_t XINPUT_BTN_BACK           = 0x0020;
constexpr uint16_t XINPUT_BTN_LEFT_THUMB     = 0x0040;
constexpr uint16_t XINPUT_BTN_RIGHT_THUMB    = 0x0080;
constexpr uint16_t XINPUT_BTN_LEFT_SHOULDER  = 0x0100;
constexpr uint16_t XINPUT_BTN_RIGHT_SHOULDER = 0x0200;
constexpr uint16_t XINPUT_BTN_A              = 0x1000;
constexpr uint16_t XINPUT_BTN_B              = 0x2000;
constexpr uint16_t XINPUT_BTN_X              = 0x4000;
constexpr uint16_t XINPUT_BTN_Y              = 0x8000;

#pragma pack(push, 1)
struct WinXInputGamepad {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};

struct WinXInputState {
    uint32_t dwPacketNumber;
    WinXInputGamepad gamepad;
};
#pragma pack(pop)

// Cập nhật nút bấm tay cầm từ sự kiện KeyEvent của Android
void XInputBridge_UpdateButton(int32_t androidKeyCode, bool isDown);

// Cập nhật trục cần gạt Analog, D-Pad HAT và cò L2/R2 từ MotionEvent của Android
void XInputBridge_UpdateAxes(
    float leftX,
    float leftY,
    float rightX,
    float rightY,
    float leftTrigger,
    float rightTrigger,
    float hatX,
    float hatY
);

// Đặt trạng thái kết nối tay cầm vật lý
void XInputBridge_SetConnected(bool connected);

// Hàm hook IAT cho XINPUT1_4.dll
uint32_t WINAPI XInputBridge_GetState(uint32_t dwUserIndex, void* pState);
uint32_t WINAPI XInputBridge_SetState(uint32_t dwUserIndex, void* pVibration);

#ifdef __cplusplus
}
#endif
