#include "dinput_bridge.h"
#include "d3d11_bridge.h"
#include "win32_bridge.h"
#include <android/log.h>
#include <cstring>
#include <cstdint>

#define LOG_TAG "SGRE_DInputBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static uint64_t gDirectInput8Vtable[32] = {0};
static uint64_t gDirectInputDevice8Vtable[32] = {0};

static SGRE_COM_Mock gMockDirectInput = { gDirectInput8Vtable };
static SGRE_COM_Mock gMockDIDevice = { gDirectInputDevice8Vtable };

static int64_t WINAPI mock_DI_QueryInterface(void* thisPtr, const void*, void** ppvObject) {
    if (ppvObject) *ppvObject = thisPtr;
    return 0; // S_OK
}
static uint32_t WINAPI mock_DI_AddRef(void*) { return 1; }
static uint32_t WINAPI mock_DI_Release(void*) { return 1; }
static int64_t WINAPI mock_DI_Generic(void*, void*, void*, void*, void*, void*, void*, void*) { return 0; }

// IDirectInput8W::CreateDevice(this, rguid, lplpDirectInputDevice, pUnkOuter)
static int64_t WINAPI mock_DI_CreateDevice(void*, const void*, void** lplpDirectInputDevice, void*) {
    LOGI("IDirectInput8::CreateDevice called");
    if (lplpDirectInputDevice) {
        *lplpDirectInputDevice = &gMockDIDevice;
    }
    return 0; // DI_OK = 0
}

// IDirectInputDevice8W methods
// Slot 7: Acquire
static int64_t WINAPI mock_DIDevice_Acquire(void*) {
    LOGI("IDirectInputDevice8::Acquire called");
    return 0;
}
// Slot 8: Unacquire
static int64_t WINAPI mock_DIDevice_Unacquire(void*) { return 0; }
// Slot 9: GetDeviceState(this, cbData, lpvData)
static int64_t WINAPI mock_DIDevice_GetDeviceState(void*, uint32_t cbData, void* lpvData) {
    if (lpvData && cbData > 0) {
        memset(lpvData, 0, cbData);
        uint8_t* p = reinterpret_cast<uint8_t*>(lpvData);

        // Ánh xạ cho Keyboard (cbData == 256)
        if (cbData == 256) {
            bool enterPressed = Win32Bridge_GetKeyState(0x0D) || Win32Bridge_GetKeyState(0x01); // VK_RETURN hoặc VK_LBUTTON
            bool spacePressed = Win32Bridge_GetKeyState(0x20) || Win32Bridge_GetKeyState(0x01); // VK_SPACE
            bool escPressed = Win32Bridge_GetKeyState(0x1B);                                   // VK_ESCAPE
            bool upPressed = Win32Bridge_GetKeyState(0x26);                                    // VK_UP
            bool downPressed = Win32Bridge_GetKeyState(0x28);                                  // VK_DOWN
            bool leftPressed = Win32Bridge_GetKeyState(0x25);                                  // VK_LEFT
            bool rightPressed = Win32Bridge_GetKeyState(0x27);                                 // VK_RIGHT

            if (enterPressed) p[0x1C] = 0x80; // DIK_RETURN
            if (spacePressed) p[0x39] = 0x80; // DIK_SPACE
            if (escPressed) p[0x01] = 0x80;   // DIK_ESCAPE
            if (upPressed) p[0xC8] = 0x80;    // DIK_UP
            if (downPressed) p[0xD0] = 0x80;  // DIK_DOWN
            if (leftPressed) p[0xCB] = 0x80;  // DIK_LEFT
            if (rightPressed) p[0xCD] = 0x80; // DIK_RIGHT
        } else if (cbData >= 48) {
            // Ánh xạ Joystick Buttons (DIJOYSTATE: buttons bắt đầu từ offset 48)
            bool buttonAPressed = Win32Bridge_GetKeyState(0x0D) || Win32Bridge_GetKeyState(0x01) || Win32Bridge_GetKeyState(0x20);
            if (buttonAPressed && cbData > 48) {
                p[48] = 0x80; // Button 0 (A / Enter)
            }
        }
    }
    return 0; // DI_OK
}
// Slot 10: GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags)
static int64_t WINAPI mock_DIDevice_GetDeviceData(void*, uint32_t, void*, uint32_t* pdwInOut, uint32_t) {
    if (pdwInOut) *pdwInOut = 0;
    return 0;
}
// Slot 11: SetDataFormat
static int64_t WINAPI mock_DIDevice_SetDataFormat(void*, const void*) {
    LOGI("IDirectInputDevice8::SetDataFormat called");
    return 0;
}
// Slot 13: SetCooperativeLevel
static int64_t WINAPI mock_DIDevice_SetCooperativeLevel(void*, void*, uint32_t) {
    LOGI("IDirectInputDevice8::SetCooperativeLevel called");
    return 0;
}

static void initDInputVtablesOnce() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    for (int i = 0; i < 32; ++i) {
        gDirectInput8Vtable[i] = reinterpret_cast<uint64_t>(&mock_DI_Generic);
        gDirectInputDevice8Vtable[i] = reinterpret_cast<uint64_t>(&mock_DI_Generic);
    }

    // IDirectInput8W slots
    gDirectInput8Vtable[0] = reinterpret_cast<uint64_t>(&mock_DI_QueryInterface);
    gDirectInput8Vtable[1] = reinterpret_cast<uint64_t>(&mock_DI_AddRef);
    gDirectInput8Vtable[2] = reinterpret_cast<uint64_t>(&mock_DI_Release);
    gDirectInput8Vtable[3] = reinterpret_cast<uint64_t>(&mock_DI_CreateDevice);

    // IDirectInputDevice8W slots
    gDirectInputDevice8Vtable[0] = reinterpret_cast<uint64_t>(&mock_DI_QueryInterface);
    gDirectInputDevice8Vtable[1] = reinterpret_cast<uint64_t>(&mock_DI_AddRef);
    gDirectInputDevice8Vtable[2] = reinterpret_cast<uint64_t>(&mock_DI_Release);
    gDirectInputDevice8Vtable[7] = reinterpret_cast<uint64_t>(&mock_DIDevice_Acquire);
    gDirectInputDevice8Vtable[8] = reinterpret_cast<uint64_t>(&mock_DIDevice_Unacquire);
    gDirectInputDevice8Vtable[9] = reinterpret_cast<uint64_t>(&mock_DIDevice_GetDeviceState);
    gDirectInputDevice8Vtable[10] = reinterpret_cast<uint64_t>(&mock_DIDevice_GetDeviceData);
    gDirectInputDevice8Vtable[11] = reinterpret_cast<uint64_t>(&mock_DIDevice_SetDataFormat);
    gDirectInputDevice8Vtable[13] = reinterpret_cast<uint64_t>(&mock_DIDevice_SetCooperativeLevel);
}

extern "C" {
int32_t WINAPI DirectInput8Create(
    void* /* hinst */,
    uint32_t dwVersion,
    const void* /* riidltf */,
    void** ppvOut,
    void* /* punkOuter */
) {
    initDInputVtablesOnce();
    LOGI("DirectInput8Create intercepted: Version=0x%X, ppvOut=%p", dwVersion, ppvOut);
    if (ppvOut) {
        *ppvOut = &gMockDirectInput;
        LOGI("-> Đã cấp phát Mock IDirectInput8: %p", *ppvOut);
    }
    return 0; // DI_OK = 0
}
}
