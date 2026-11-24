#include "dinput_bridge.h"
#include "win32_bridge.h"
#include <android/log.h>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <algorithm>

#define LOG_TAG "SGRE_DInputBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

struct DI_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool matches(const void* ptr) const {
        return ptr && memcmp(this, ptr, sizeof(DI_GUID)) == 0;
    }
};

static const DI_GUID GUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const DI_GUID GUID_SysMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

enum class MousePhase {
    Idle,
    WakeMove,   // Frame 1: dịch chuyển nhẹ và di chuột để kích hoạt _mouseMode = true
    HoverFocus, // Frame 2: hover đúng toạ độ nút để _motionMouseFocus nhận diện nút
    PressHold,  // Frame 3: nhấn chuột trái (0x80)
    Release     // Frame 4: nhả chuột trái để engine kích hoạt onPress
};

struct TouchMouseState {
    std::mutex mutex;
    MousePhase phase = MousePhase::Idle;
    int32_t currentX = 960;
    int32_t currentY = 540;
    int32_t lastReportedX = 960;
    int32_t lastReportedY = 540;
    bool isTouchDown = false;
    bool hasPendingTouchUp = false;

    // Right Click (chạm 2 ngón)
    int rightClickFrames = 0;

    // Wheel
    int32_t pendingWheelZ = 0;
};

static TouchMouseState sTouchMouse;

struct KeyPulseState {
    std::atomic<bool> isHeld{false};
    std::atomic<int32_t> framesRemaining{0};
};
static KeyPulseState sKeyPulses[256];

} // namespace

void DInputBridge_PostKeyEvent(uint32_t virtualKey, bool isDown) {
    if (virtualKey >= 256) return;
    if (isDown) {
        sKeyPulses[virtualKey].isHeld.store(true);
        sKeyPulses[virtualKey].framesRemaining.store(4); // Giữ tối thiểu 4 frames (~66ms) để DirectInput polling chắc chắn ghi nhận
        Win32Bridge_SetKeyState(static_cast<uint8_t>(virtualKey), true);
    } else {
        sKeyPulses[virtualKey].isHeld.store(false);
        // Không xóa ngay framesRemaining, để vòng lặp GetDeviceState đếm lùi
    }
}

void DInputBridge_PostTouchAction(int32_t action, int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(sTouchMouse.mutex);
    sTouchMouse.currentX = x;
    sTouchMouse.currentY = y;

    switch (action) {
        case 0: // TouchDown
            sTouchMouse.isTouchDown = true;
            sTouchMouse.hasPendingTouchUp = false;
            // Bắt đầu chuỗi bước: WakeMove -> HoverFocus -> PressHold
            sTouchMouse.phase = MousePhase::WakeMove;
            break;
        case 1: // TouchMove
            sTouchMouse.currentX = x;
            sTouchMouse.currentY = y;
            break;
        case 2: // TouchUp
            sTouchMouse.isTouchDown = false;
            if (sTouchMouse.phase == MousePhase::PressHold) {
                sTouchMouse.phase = MousePhase::Release;
            } else {
                sTouchMouse.hasPendingTouchUp = true;
            }
            break;
        case 7: // RightClick
            sTouchMouse.rightClickFrames = 3;
            break;
        default:
            break;
    }
}

void DInputBridge_PostMouseWheel(int32_t wheelDelta) {
    std::lock_guard<std::mutex> lock(sTouchMouse.mutex);
    sTouchMouse.pendingWheelZ += wheelDelta;
}

namespace {

static uint64_t gDirectInput8Vtable[32] = {0};
static uint64_t gDIDeviceKeyboardVtable[32] = {0};
static uint64_t gDIDeviceMouseVtable[32] = {0};

struct DI_COM_Mock {
    void* vtable;
};

static DI_COM_Mock gMockDirectInput = { gDirectInput8Vtable };
static DI_COM_Mock gMockDIDeviceKeyboard = { gDIDeviceKeyboardVtable };
static DI_COM_Mock gMockDIDeviceMouse = { gDIDeviceMouseVtable };

static int64_t WINAPI mock_DI_QueryInterface(void* thisPtr, const void*, void** ppvObject) {
    if (ppvObject) *ppvObject = thisPtr;
    return 0; // S_OK
}
static uint32_t WINAPI mock_DI_AddRef(void*) { return 1; }
static uint32_t WINAPI mock_DI_Release(void*) { return 1; }
static int64_t WINAPI mock_DI_Generic(void*, void*, void*, void*, void*, void*, void*, void*) { return 0; }

// IDirectInput8W::CreateDevice
static int64_t WINAPI mock_DI_CreateDevice(void*, const void* rguid, void** lplpDirectInputDevice, void*) {
    if (!lplpDirectInputDevice) return static_cast<int64_t>(0x80004003);

    if (GUID_SysMouse.matches(rguid)) {
        LOGI("IDirectInput8::CreateDevice -> Created Mouse Device");
        *lplpDirectInputDevice = &gMockDIDeviceMouse;
    } else {
        LOGI("IDirectInput8::CreateDevice -> Created Keyboard Device");
        *lplpDirectInputDevice = &gMockDIDeviceKeyboard;
    }
    return 0; // DI_OK
}

// Keyboard GetDeviceState (cbData == 256)
static int64_t WINAPI mock_DIKeyboard_GetDeviceState(void*, uint32_t cbData, void* lpvData) {
    if (!lpvData || cbData == 0) return 0;
    memset(lpvData, 0, cbData);
    uint8_t* p = reinterpret_cast<uint8_t*>(lpvData);

    if (cbData >= 256) {
        auto checkAndAdvanceKey = [](uint32_t vk) -> bool {
            if (vk >= 256) return false;
            bool held = sKeyPulses[vk].isHeld.load();
            int32_t rem = sKeyPulses[vk].framesRemaining.load();
            if (rem > 0) {
                sKeyPulses[vk].framesRemaining.store(rem - 1);
            }
            bool active = held || (rem > 0);
            if (!active) {
                Win32Bridge_SetKeyState(static_cast<uint8_t>(vk), false);
            }
            return active;
        };

        // Ánh xạ toàn diện VirtualKey sang DIK theo bảng rsc_project/script/keybind.nut của M2 Engine:
        if (checkAndAdvanceKey(0x0D)) { p[0x1C] = 0x80; p[0x39] = 0x80; } // VK_RETURN -> DIK_RETURN, DIK_SPACE (A button)
        if (checkAndAdvanceKey(0x20)) { p[0x39] = 0x80; }                 // VK_SPACE  -> DIK_SPACE (A button)
        if (checkAndAdvanceKey(0x08)) { p[0x0E] = 0x80; p[0x2D] = 0x80; } // VK_BACK   -> DIK_BACK, DIK_X (B button / Back / Cancel)
        if (checkAndAdvanceKey(0x1B)) { p[0x01] = 0x80; }                 // VK_ESCAPE -> DIK_ESCAPE (System Menu / Exit Game)
        if (checkAndAdvanceKey(0x11)) { p[0x1D] = 0x80; p[0x12] = 0x80; } // VK_CONTROL-> DIK_LCONTROL, DIK_E (R1 / Fast Forward Skip)
        if (checkAndAdvanceKey(0x26)) { p[0xC8] = 0x80; }                 // VK_UP     -> DIK_UP
        if (checkAndAdvanceKey(0x28)) { p[0xD0] = 0x80; }                 // VK_DOWN   -> DIK_DOWN
        if (checkAndAdvanceKey(0x25)) { p[0xCB] = 0x80; }                 // VK_LEFT   -> DIK_LEFT
        if (checkAndAdvanceKey(0x27)) { p[0xCD] = 0x80; }                 // VK_RIGHT  -> DIK_RIGHT
        if (checkAndAdvanceKey(0x58)) { p[0x2D] = 0x80; }                 // 'X'       -> DIK_X (B button)
        if (checkAndAdvanceKey(0x5A)) { p[0x2C] = 0x80; }                 // 'Z'       -> DIK_Z (L2 button)
        if (checkAndAdvanceKey(0x43)) { p[0x2E] = 0x80; }                 // 'C'       -> DIK_C (R2 button)
        if (checkAndAdvanceKey(0x51)) { p[0x10] = 0x80; }                 // 'Q'       -> DIK_Q (L1/LB Page Prev)
        if (checkAndAdvanceKey(0x45)) { p[0x12] = 0x80; }                 // 'E'       -> DIK_E (R1/RB Page Next)
        if (checkAndAdvanceKey(0x31)) { p[0x02] = 0x80; }                 // '1'       -> DIK_1 (START button)
        if (checkAndAdvanceKey(0x41)) { p[0x1E] = 0x80; }                 // 'A'       -> DIK_A
        if (checkAndAdvanceKey(0x53)) { p[0x1F] = 0x80; }                 // 'S'       -> DIK_S
        if (checkAndAdvanceKey(0x44)) { p[0x20] = 0x80; }                 // 'D'       -> DIK_D
        if (checkAndAdvanceKey(0xBC)) { p[0x33] = 0x80; }                 // ','       -> DIK_COMMA
        if (checkAndAdvanceKey(0xBE)) { p[0x34] = 0x80; }                 // '.'       -> DIK_PERIOD
        if (checkAndAdvanceKey(0xDB)) { p[0x1A] = 0x80; }                 // '['       -> DIK_LBRACKET (R_UP)
        if (checkAndAdvanceKey(0xDD)) { p[0x1B] = 0x80; }                 // ']'       -> DIK_RBRACKET (R_DOWN)
        if (checkAndAdvanceKey(0x2D)) { p[0xD2] = 0x80; }                 // VK_INSERT -> DIK_INSERT (Y button / Default)
        if (checkAndAdvanceKey(0x2E)) { p[0xD3] = 0x80; }                 // VK_DELETE -> DIK_DELETE (X button)
        for (uint32_t fIdx = 0; fIdx < 10; ++fIdx) {
            if (checkAndAdvanceKey(0x70 + fIdx)) {
                p[0x3B + fIdx] = 0x80;                                    // VK_F1..F10 -> DIK_F1..F10
            }
        }
        if (checkAndAdvanceKey(0x7A)) { p[0x57] = 0x80; }                 // VK_F11    -> DIK_F11
        if (checkAndAdvanceKey(0x7B)) { p[0x58] = 0x80; }                 // VK_F12    -> DIK_F12
    }
    return 0; // DI_OK
}

// Mouse GetDeviceState (cbData == 20 DIMOUSESTATE2 hoặc 16 DIMOUSESTATE) (IDA 0x1404F4760)
static int64_t WINAPI mock_DIMouse_GetDeviceState(void*, uint32_t cbData, void* lpvData) {
    if (!lpvData || cbData == 0) return 0;
    memset(lpvData, 0, cbData);
    uint8_t* p = reinterpret_cast<uint8_t*>(lpvData);

    std::lock_guard<std::mutex> lock(sTouchMouse.mutex);

    int32_t deltaX = sTouchMouse.currentX - sTouchMouse.lastReportedX;
    int32_t deltaY = sTouchMouse.currentY - sTouchMouse.lastReportedY;

    // Cập nhật CursorPos trong Win32 Bridge để game đọc toạ độ chuột tuyệt đối
    Win32Bridge_SetCursorPos(sTouchMouse.currentX, sTouchMouse.currentY);

    uint8_t leftButton = 0;
    uint8_t rightButton = 0;

    switch (sTouchMouse.phase) {
        case MousePhase::WakeMove:
            // Báo delta tối thiểu 1 pixel để game phát hiện di chuyển chuột và bật _mouseMode = true
            deltaX = (deltaX != 0) ? deltaX : 1;
            deltaY = (deltaY != 0) ? deltaY : 1;
            leftButton = 0;
            sTouchMouse.phase = MousePhase::HoverFocus;
            break;

        case MousePhase::HoverFocus:
            // Giữ hover tại nút, buttons = 0 để _motionMouseFocus nhận diện nút
            leftButton = 0;
            sTouchMouse.phase = MousePhase::PressHold;
            break;

        case MousePhase::PressHold:
            // Nhấn chuột trái
            leftButton = 0x80;
            Win32Bridge_SetKeyState(0x01, true); // VK_LBUTTON
            if (!sTouchMouse.isTouchDown || sTouchMouse.hasPendingTouchUp) {
                sTouchMouse.phase = MousePhase::Release;
                sTouchMouse.hasPendingTouchUp = false;
            }
            break;

        case MousePhase::Release:
            // Nhả chuột trái -> Part::onPress được kích hoạt!
            leftButton = 0;
            Win32Bridge_SetKeyState(0x01, false);
            sTouchMouse.phase = MousePhase::Idle;
            break;

        case MousePhase::Idle:
        default:
            leftButton = 0;
            Win32Bridge_SetKeyState(0x01, false);
            break;
    }

    if (sTouchMouse.rightClickFrames > 0) {
        rightButton = 0x80;
        Win32Bridge_SetKeyState(0x02, true); // VK_RBUTTON
        sTouchMouse.rightClickFrames--;
        if (sTouchMouse.rightClickFrames == 0) {
            Win32Bridge_SetKeyState(0x02, false);
        }
    }

    sTouchMouse.lastReportedX = sTouchMouse.currentX;
    sTouchMouse.lastReportedY = sTouchMouse.currentY;

    // Ghi vào DIMOUSESTATE / DIMOUSESTATE2
    // +0: lX (int32_t)
    *reinterpret_cast<int32_t*>(p + 0) = deltaX;
    // +4: lY (int32_t)
    *reinterpret_cast<int32_t*>(p + 4) = deltaY;
    // +8: lZ (int32_t) Wheel
    *reinterpret_cast<int32_t*>(p + 8) = sTouchMouse.pendingWheelZ;
    sTouchMouse.pendingWheelZ = 0;

    // +12..19: rgbButtons[8]
    if (cbData > 12) p[12] = leftButton;
    if (cbData > 13) p[13] = rightButton;

    return 0; // DI_OK
}

static void initDInputVtablesOnce() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    for (int i = 0; i < 32; ++i) {
        gDirectInput8Vtable[i] = reinterpret_cast<uint64_t>(&mock_DI_Generic);
        gDIDeviceKeyboardVtable[i] = reinterpret_cast<uint64_t>(&mock_DI_Generic);
        gDIDeviceMouseVtable[i] = reinterpret_cast<uint64_t>(&mock_DI_Generic);
    }

    // IDirectInput8W slots
    gDirectInput8Vtable[0] = reinterpret_cast<uint64_t>(&mock_DI_QueryInterface);
    gDirectInput8Vtable[1] = reinterpret_cast<uint64_t>(&mock_DI_AddRef);
    gDirectInput8Vtable[2] = reinterpret_cast<uint64_t>(&mock_DI_Release);
    gDirectInput8Vtable[3] = reinterpret_cast<uint64_t>(&mock_DI_CreateDevice);

    // Keyboard slots
    gDIDeviceKeyboardVtable[0] = reinterpret_cast<uint64_t>(&mock_DI_QueryInterface);
    gDIDeviceKeyboardVtable[1] = reinterpret_cast<uint64_t>(&mock_DI_AddRef);
    gDIDeviceKeyboardVtable[2] = reinterpret_cast<uint64_t>(&mock_DI_Release);
    gDIDeviceKeyboardVtable[9] = reinterpret_cast<uint64_t>(&mock_DIKeyboard_GetDeviceState);

    // Mouse slots
    gDIDeviceMouseVtable[0] = reinterpret_cast<uint64_t>(&mock_DI_QueryInterface);
    gDIDeviceMouseVtable[1] = reinterpret_cast<uint64_t>(&mock_DI_AddRef);
    gDIDeviceMouseVtable[2] = reinterpret_cast<uint64_t>(&mock_DI_Release);
    gDIDeviceMouseVtable[9] = reinterpret_cast<uint64_t>(&mock_DIMouse_GetDeviceState);
}

} // namespace

extern "C" {

int32_t WINAPI DirectInput8Create(
    void*, uint32_t dwVersion, const void*, void** ppvOut, void*
) {
    initDInputVtablesOnce();
    LOGI("DirectInput8Create intercepted: Version=0x%X, ppvOut=%p", dwVersion, ppvOut);
    if (ppvOut) {
        *ppvOut = &gMockDirectInput;
    }
    return 0; // DI_OK
}

}
