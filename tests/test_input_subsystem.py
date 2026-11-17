import unittest

class TestInputSubsystemLogic(unittest.TestCase):
    """
    Kiểm thử đơn vị cho hệ thống input subsystem kết nối giữa Android,
    Win32 Bridge và DirectInput 8.
    """

    def setUp(self):
        # Mô phỏng mảng trạng thái 256 phím của Win32
        self.key_states = [0] * 256
        self.cursor_pos = [960, 540]

    def set_key_state(self, vk, is_down):
        if 0 <= vk < 256:
            self.key_states[vk] = 1 if is_down else 0

    def get_key_state(self, vk):
        if 0 <= vk < 256:
            return 0x8000 if self.key_states[vk] else 0
        return 0

    def set_cursor_pos(self, x, y):
        self.cursor_pos[0] = x
        self.cursor_pos[1] = y

    def get_cursor_pos(self):
        return tuple(self.cursor_pos)

    def map_android_keycode_to_win32(self, keycode):
        mapping = {
            66: 0x0D,   # KEYCODE_ENTER -> VK_RETURN (A button)
            160: 0x0D,  # KEYCODE_NUMPAD_ENTER -> VK_RETURN (A button)
            96: 0x0D,   # KEYCODE_BUTTON_A -> VK_RETURN (A button)
            23: 0x0D,   # KEYCODE_DPAD_CENTER -> VK_RETURN (A button)
            62: 0x20,   # KEYCODE_SPACE -> VK_SPACE (A button)
            4: 0x08,    # KEYCODE_BACK -> VK_BACK (B button / Back / Cancel)
            97: 0x08,   # KEYCODE_BUTTON_B -> VK_BACK (B button / Back / Cancel)
            67: 0x08,   # KEYCODE_DEL -> VK_BACK (B button / Back / Cancel)
            111: 0x1B,  # KEYCODE_ESCAPE -> VK_ESCAPE (System Menu / Exit Dialog)
            19: 0x26,   # KEYCODE_DPAD_UP -> VK_UP
            20: 0x28,   # KEYCODE_DPAD_DOWN -> VK_DOWN
            21: 0x25,   # KEYCODE_DPAD_LEFT -> VK_LEFT
            22: 0x27,   # KEYCODE_DPAD_RIGHT -> VK_RIGHT
            99: 0x2E,   # KEYCODE_BUTTON_X -> VK_DELETE (X button)
            100: 0x2D,  # KEYCODE_BUTTON_Y -> VK_INSERT (Y button)
            109: 0x2D,  # KEYCODE_BUTTON_SELECT -> VK_INSERT (Y button)
            102: 0x51,  # KEYCODE_BUTTON_L1 -> 'Q' (L1 button)
            92: 0x51,   # KEYCODE_PAGE_UP -> 'Q' (L1 button)
            103: 0x45,  # KEYCODE_BUTTON_R1 -> 'E' (R1 button)
            93: 0x45,   # KEYCODE_PAGE_DOWN -> 'E' (R1 button)
            104: 0x5A,  # KEYCODE_BUTTON_L2 -> 'Z' (L2 button)
            105: 0x43,  # KEYCODE_BUTTON_R2 -> 'C' (R2 button)
            108: 0x31,  # KEYCODE_BUTTON_START -> '1' (START button)
            82: 0x31,   # KEYCODE_MENU -> '1' (START button)
            113: 0x11,  # KEYCODE_CTRL_LEFT -> VK_CONTROL
            114: 0x11,  # KEYCODE_CTRL_RIGHT -> VK_CONTROL
        }
        if keycode in mapping:
            return mapping[keycode]
        if 29 <= keycode <= 54:
            return 0x41 + (keycode - 29)
        return 0

    def mock_dinput_get_device_state(self, cb_data):
        buf = bytearray(cb_data)
        if cb_data == 256:
            # Bàn phím DirectInput - Phục vụ M2 Engine theo keybind.nut
            enter_pressed = (self.get_key_state(0x0D) != 0)
            space_pressed = (self.get_key_state(0x20) != 0)
            back_pressed  = (self.get_key_state(0x08) != 0) or (self.get_key_state(0x58) != 0)
            esc_pressed   = (self.get_key_state(0x1B) != 0)
            ctrl_pressed  = (self.get_key_state(0x11) != 0)
            up_pressed    = (self.get_key_state(0x26) != 0)
            down_pressed  = (self.get_key_state(0x28) != 0)
            left_pressed  = (self.get_key_state(0x25) != 0)
            right_pressed = (self.get_key_state(0x27) != 0)
            btn_x_pressed = (self.get_key_state(0x2E) != 0)
            btn_y_pressed = (self.get_key_state(0x2D) != 0)
            btn_l1_pressed= (self.get_key_state(0x51) != 0)
            btn_r1_pressed= (self.get_key_state(0x45) != 0)
            start_pressed = (self.get_key_state(0x31) != 0)

            if enter_pressed:
                buf[0x1C] = 0x80 # DIK_RETURN
                buf[0x39] = 0x80 # DIK_SPACE
            if space_pressed:
                buf[0x39] = 0x80 # DIK_SPACE
            if back_pressed:
                buf[0x0E] = 0x80 # DIK_BACK (B button in game)
                buf[0x2D] = 0x80 # DIK_X (B button in game)
            if esc_pressed:   buf[0x01] = 0x80 # DIK_ESCAPE
            if ctrl_pressed:
                buf[0x1D] = 0x80 # DIK_LCONTROL
                buf[0x12] = 0x80 # DIK_E (R1 skip)
            if up_pressed:    buf[0xC8] = 0x80 # DIK_UP
            if down_pressed:  buf[0xD0] = 0x80 # DIK_DOWN
            if left_pressed:  buf[0xCB] = 0x80 # DIK_LEFT
            if right_pressed: buf[0xCD] = 0x80 # DIK_RIGHT
            if btn_x_pressed: buf[0xD3] = 0x80 # DIK_DELETE (X button)
            if btn_y_pressed: buf[0xD2] = 0x80 # DIK_INSERT (Y button)
            if btn_l1_pressed:buf[0x10] = 0x80 # DIK_Q (L1/LB)
            if btn_r1_pressed:buf[0x12] = 0x80 # DIK_E (R1/RB)
            if start_pressed: buf[0x02] = 0x80 # DIK_1 (START)
        elif cb_data >= 48:
            # DIJOYSTATE: Buttons bắt đầu từ offset 48
            btn_a = (self.get_key_state(0x0D) != 0) or (self.get_key_state(0x20) != 0)
            if btn_a and cb_data > 48:
                buf[48] = 0x80 # Button 0
        return buf

    def test_android_to_win32_key_mapping(self):
        # Kiểm thử Enter, Button A và DPad Center ánh xạ sang VK_RETURN (0x0D)
        self.assertEqual(self.map_android_keycode_to_win32(66), 0x0D)
        self.assertEqual(self.map_android_keycode_to_win32(96), 0x0D)
        self.assertEqual(self.map_android_keycode_to_win32(23), 0x0D)

        # Kiểm thử Space
        self.assertEqual(self.map_android_keycode_to_win32(62), 0x20)

        # Kiểm thử Back và Button B ánh xạ sang VK_BACK (0x08) cho nút B trong game
        self.assertEqual(self.map_android_keycode_to_win32(4), 0x08)
        self.assertEqual(self.map_android_keycode_to_win32(97), 0x08)
        self.assertEqual(self.map_android_keycode_to_win32(67), 0x08)

        # Kiểm thử Escape (phím cứng bàn phím) ánh xạ sang VK_ESCAPE (0x1B)
        self.assertEqual(self.map_android_keycode_to_win32(111), 0x1B)

        # Kiểm thử D-pad
        self.assertEqual(self.map_android_keycode_to_win32(19), 0x26)
        self.assertEqual(self.map_android_keycode_to_win32(20), 0x28)
        self.assertEqual(self.map_android_keycode_to_win32(21), 0x25)
        self.assertEqual(self.map_android_keycode_to_win32(22), 0x27)

        # Kiểm thử Gamepad Buttons X, Y, L1, R1, Start
        self.assertEqual(self.map_android_keycode_to_win32(99), 0x2E) # X -> VK_DELETE
        self.assertEqual(self.map_android_keycode_to_win32(100), 0x2D) # Y -> VK_INSERT
        self.assertEqual(self.map_android_keycode_to_win32(102), 0x51) # L1 -> 'Q'
        self.assertEqual(self.map_android_keycode_to_win32(103), 0x45) # R1 -> 'E'
        self.assertEqual(self.map_android_keycode_to_win32(108), 0x31) # START -> '1'

        # Kiểm thử các phím dị thường / ngoài danh sách
        self.assertEqual(self.map_android_keycode_to_win32(-1), 0)
        self.assertEqual(self.map_android_keycode_to_win32(999), 0)

    def test_win32_key_state_persistence(self):
        # Trạng thái ban đầu: chưa phím nào được bấm
        self.assertEqual(self.get_key_state(0x0D), 0)
        self.assertEqual(self.get_key_state(0x20), 0)

        # Bấm phím Return
        self.set_key_state(0x0D, True)
        self.assertEqual(self.get_key_state(0x0D), 0x8000)

        # Nhả phím Return
        self.set_key_state(0x0D, False)
        self.assertEqual(self.get_key_state(0x0D), 0)

        # Kiểm thử trường hợp biên chỉ số ngoài khoảng 0..255
        self.assertEqual(self.get_key_state(-5), 0)
        self.assertEqual(self.get_key_state(300), 0)

    def test_cursor_position_updates(self):
        self.assertEqual(self.get_cursor_pos(), (960, 540))
        self.set_cursor_pos(1280, 720)
        self.assertEqual(self.get_cursor_pos(), (1280, 720))

    def test_dinput_keyboard_state_synchronization(self):
        # Khi nhấn VK_RETURN (A button)
        self.set_key_state(0x0D, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x1C], 0x80, "DIK_RETURN phải mang giá trị 0x80 khi VK_RETURN được nhấn")
        self.assertEqual(buf[0x39], 0x80, "DIK_SPACE cũng đồng thời mang 0x80 cho nút A")

        # Khi nhấn VK_BACK (B button / Back)
        self.set_key_state(0x0D, False)
        self.set_key_state(0x08, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x0E], 0x80, "DIK_BACK phải mang giá trị 0x80 khi VK_BACK được nhấn (nút B)")
        self.assertEqual(buf[0x2D], 0x80, "DIK_X phải mang giá trị 0x80 cho nút B")

        # Khi nhấn VK_SPACE
        self.set_key_state(0x08, False)
        self.set_key_state(0x20, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x1C], 0x00)
        self.assertEqual(buf[0x39], 0x80)

        # Khi nhấn nút Start (0x31)
        self.set_key_state(0x20, False)
        self.set_key_state(0x31, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x02], 0x80, "DIK_1 phải mang giá trị 0x80 cho nút START")

    def test_dinput_joystick_button_synchronization(self):
        # Khi chưa nhấn
        buf = self.mock_dinput_get_device_state(80)
        self.assertEqual(buf[48], 0x00)

        # Khi bấm phím Enter / Button A
        self.set_key_state(0x0D, True)
        buf = self.mock_dinput_get_device_state(80)
        self.assertEqual(buf[48], 0x80, "Button 0 (A) trên gamepad ảo phải được kích hoạt khi nhấn Enter")

    def test_gesture_actions_and_back_button_interception(self):
        # Kiểm thử ánh xạ cử chỉ cảm ứng sang các thông điệp Win32 / DirectInput
        ACTION_TOUCH_DOWN = 0
        ACTION_TOUCH_MOVE = 1
        ACTION_TOUCH_UP = 2
        ACTION_SWIPE_UP = 3
        ACTION_SWIPE_DOWN = 4
        ACTION_SWIPE_RIGHT = 5
        ACTION_LONG_PRESS = 6
        ACTION_RIGHT_CLICK = 7

        events_dispatched = []

        def mock_inject_touch_event(action, norm_x, norm_y):
            target_x = int(min(max(norm_x, 0.0), 1.0) * 1920)
            target_y = int(min(max(norm_y, 0.0), 1.0) * 1080)

            if action == ACTION_SWIPE_UP:
                events_dispatched.append(("WHEEL", 120))
                events_dispatched.append(("KEY_DOWN", 0x26)) # VK_UP
                events_dispatched.append(("KEY_UP", 0x26))
            elif action == ACTION_SWIPE_DOWN:
                events_dispatched.append(("WHEEL", -120))
                events_dispatched.append(("KEY_DOWN", 0x28)) # VK_DOWN
                events_dispatched.append(("KEY_UP", 0x28))
            elif action == ACTION_SWIPE_RIGHT:
                events_dispatched.append(("RIGHT_CLICK", target_x, target_y))
            elif action == ACTION_LONG_PRESS:
                events_dispatched.append(("KEY_DOWN", 0x11)) # VK_CONTROL
            elif action == ACTION_RIGHT_CLICK:
                events_dispatched.append(("RIGHT_CLICK", target_x, target_y))

        # 1. Vuốt lên: Kích hoạt Wheel Up (120) và VK_UP để mở Backlog
        events_dispatched.clear()
        mock_inject_touch_event(ACTION_SWIPE_UP, 0.5, 0.5)
        self.assertEqual(events_dispatched, [("WHEEL", 120), ("KEY_DOWN", 0x26), ("KEY_UP", 0x26)])

        # 2. Vuốt xuống: Kích hoạt Wheel Down (-120) và VK_DOWN để cuộn/đóng
        events_dispatched.clear()
        mock_inject_touch_event(ACTION_SWIPE_DOWN, 0.5, 0.5)
        self.assertEqual(events_dispatched, [("WHEEL", -120), ("KEY_DOWN", 0x28), ("KEY_UP", 0x28)])

        # 3. Vuốt phải: Kích hoạt Right Click ẩn hộp thoại
        events_dispatched.clear()
        mock_inject_touch_event(ACTION_SWIPE_RIGHT, 0.8, 0.6)
        self.assertEqual(events_dispatched, [("RIGHT_CLICK", 1536, 648)])

        # 4. Giữ ngón tay (Long Press): Kích hoạt VK_CONTROL tua nhanh
        events_dispatched.clear()
        mock_inject_touch_event(ACTION_LONG_PRESS, 0.5, 0.5)
        self.assertEqual(events_dispatched, [("KEY_DOWN", 0x11)])

        # 5. Phím Back của Android (KEYCODE_BACK = 4) được chặn và điều hướng thành nút B (KEYCODE_BUTTON_B = 97 / VK_BACK = 0x08)
        android_back_intercepted_vk = self.map_android_keycode_to_win32(97)
        self.assertEqual(android_back_intercepted_vk, 0x08, "Android Back phải chuyển thành VK_BACK cho nút B")

if __name__ == "__main__":
    unittest.main()
