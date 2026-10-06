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
            66: 0x0D,   # KEYCODE_ENTER -> VK_RETURN
            160: 0x0D,  # KEYCODE_NUMPAD_ENTER -> VK_RETURN
            96: 0x0D,   # KEYCODE_BUTTON_A -> VK_RETURN
            62: 0x20,   # KEYCODE_SPACE -> VK_SPACE
            111: 0x1B,  # KEYCODE_ESCAPE -> VK_ESCAPE
            4: 0x1B,    # KEYCODE_BACK -> VK_ESCAPE
            97: 0x1B,   # KEYCODE_BUTTON_B -> VK_ESCAPE
            19: 0x26,   # KEYCODE_DPAD_UP -> VK_UP
            20: 0x28,   # KEYCODE_DPAD_DOWN -> VK_DOWN
            21: 0x25,   # KEYCODE_DPAD_LEFT -> VK_LEFT
            22: 0x27,   # KEYCODE_DPAD_RIGHT -> VK_RIGHT
            54: 0x5A,   # KEYCODE_Z -> 'Z'
            99: 0x5A,   # KEYCODE_BUTTON_X -> 'Z'
            52: 0x58,   # KEYCODE_X -> 'X'
            100: 0x58,  # KEYCODE_BUTTON_Y -> 'X'
            31: 0x43,   # KEYCODE_C -> 'C'
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
            # Bàn phím DirectInput
            enter_pressed = (self.get_key_state(0x0D) != 0) or (self.get_key_state(0x01) != 0)
            space_pressed = (self.get_key_state(0x20) != 0) or (self.get_key_state(0x01) != 0)
            esc_pressed = (self.get_key_state(0x1B) != 0)
            up_pressed = (self.get_key_state(0x26) != 0)
            down_pressed = (self.get_key_state(0x28) != 0)
            left_pressed = (self.get_key_state(0x25) != 0)
            right_pressed = (self.get_key_state(0x27) != 0)

            if enter_pressed: buf[0x1C] = 0x80 # DIK_RETURN
            if space_pressed: buf[0x39] = 0x80 # DIK_SPACE
            if esc_pressed:   buf[0x01] = 0x80 # DIK_ESCAPE
            if up_pressed:    buf[0xC8] = 0x80 # DIK_UP
            if down_pressed:  buf[0xD0] = 0x80 # DIK_DOWN
            if left_pressed:  buf[0xCB] = 0x80 # DIK_LEFT
            if right_pressed: buf[0xCD] = 0x80 # DIK_RIGHT
        elif cb_data >= 48:
            # DIJOYSTATE: Buttons bắt đầu từ offset 48
            btn_a = (self.get_key_state(0x0D) != 0) or (self.get_key_state(0x01) != 0) or (self.get_key_state(0x20) != 0)
            if btn_a and cb_data > 48:
                buf[48] = 0x80 # Button 0
        return buf

    def test_android_to_win32_key_mapping(self):
        # Kiểm thử Enter và Button A ánh xạ sang VK_RETURN (0x0D)
        self.assertEqual(self.map_android_keycode_to_win32(66), 0x0D)
        self.assertEqual(self.map_android_keycode_to_win32(96), 0x0D)

        # Kiểm thử Space
        self.assertEqual(self.map_android_keycode_to_win32(62), 0x20)

        # Kiểm thử Escape và Back
        self.assertEqual(self.map_android_keycode_to_win32(111), 0x1B)
        self.assertEqual(self.map_android_keycode_to_win32(4), 0x1B)

        # Kiểm thử D-pad
        self.assertEqual(self.map_android_keycode_to_win32(19), 0x26)
        self.assertEqual(self.map_android_keycode_to_win32(20), 0x28)
        self.assertEqual(self.map_android_keycode_to_win32(21), 0x25)
        self.assertEqual(self.map_android_keycode_to_win32(22), 0x27)

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
        # Khi nhấn VK_RETURN
        self.set_key_state(0x0D, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x1C], 0x80, "DIK_RETURN phải mang giá trị 0x80 khi VK_RETURN được nhấn")
        self.assertEqual(buf[0x39], 0x00, "DIK_SPACE không được nhấn")

        # Khi nhấn VK_SPACE
        self.set_key_state(0x0D, False)
        self.set_key_state(0x20, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x1C], 0x00)
        self.assertEqual(buf[0x39], 0x80)

        # Khi người dùng chạm màn hình (VK_LBUTTON = 0x01)
        self.set_key_state(0x20, False)
        self.set_key_state(0x01, True)
        buf = self.mock_dinput_get_device_state(256)
        self.assertEqual(buf[0x1C], 0x80, "Chạm màn hình phải đồng bộ kích hoạt DIK_RETURN cho game visual novel")

    def test_dinput_joystick_button_synchronization(self):
        # Khi chưa nhấn
        buf = self.mock_dinput_get_device_state(80)
        self.assertEqual(buf[48], 0x00)

        # Khi chạm màn hình hoặc bấm phím
        self.set_key_state(0x01, True)
        buf = self.mock_dinput_get_device_state(80)
        self.assertEqual(buf[48], 0x80, "Button 0 (A) trên gamepad ảo phải được kích hoạt khi chạm màn hình")

if __name__ == "__main__":
    unittest.main()
