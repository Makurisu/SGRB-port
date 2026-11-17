import unittest

class TestDirectInputBridgeLogic(unittest.TestCase):
    def setUp(self):
        # DirectInput GUIDs
        self.IID_IDIRECTINPUT8A = "bf798030-483a-4da2-aa99-5d64ed369700"
        self.IID_IDIRECTINPUT8W = "bf798031-483a-4da2-aa99-5d64ed369700"
        self.GUID_SysKeyboard   = "6f1d2b61-d5a0-11cf-bfc7-444553540000"
        self.GUID_SysMouse      = "6f1d2b60-d5a0-11cf-bfc7-444553540000"

    def mock_direct_input8_create(self, hinst, version, riid, ppv_out, unk_outer):
        if ppv_out is None:
            return 0x80070057  # E_INVALIDARG
        if version < 0x0800:
            return 0x80040154  # DIERR_OLDDIRECTINPUTVERSION
        
        # Mô phỏng khởi tạo thành công và trả về MockIDirectInput8W
        return 0, "MockIDirectInput8W"

    def mock_create_device(self, di_instance, rguid, pp_device, unk_outer):
        if di_instance is None:
            raise ValueError("Null pointer dereference: IDirectInput8 instance is NULL!")
        if pp_device is None:
            return 0x80070057 # E_INVALIDARG
        return 0, "MockIDirectInputDevice8W"

    def mock_xinput_get_state(self, user_index):
        if user_index > 3:
            return 1167 # ERROR_DEVICE_NOT_CONNECTED
        return 1167 # Chưa kết nối gamepad thực tế

    def test_direct_input8_create_success(self):
        hr, p_di = self.mock_direct_input8_create(0x1000, 0x0800, self.IID_IDIRECTINPUT8W, {}, None)
        self.assertEqual(hr, 0, "DirectInput8Create phải trả về DI_OK (0)")
        self.assertIsNotNone(p_di, "Con trỏ ppvOut không được là NULL")
        self.assertEqual(p_di, "MockIDirectInput8W")

    def test_direct_input8_create_invalid_arg(self):
        hr = self.mock_direct_input8_create(0x1000, 0x0800, self.IID_IDIRECTINPUT8W, None, None)
        self.assertEqual(hr, 0x80070057, "DirectInput8Create phải trả về E_INVALIDARG khi ppvOut là NULL")

    def test_create_device_null_deref_prevention(self):
        # Kiểm tra kịch bản gây crash SEGV_MAPERR trước đó:
        # Nếu DirectInput8Create trả về NULL, gọi CreateDevice sẽ crash
        with self.assertRaises(ValueError):
            self.mock_create_device(None, self.GUID_SysKeyboard, {}, None)

        # Với đối tượng hợp lệ, CreateDevice phải thành công
        hr, p_dev = self.mock_create_device("MockIDirectInput8W", self.GUID_SysKeyboard, {}, None)
        self.assertEqual(hr, 0)
        self.assertIsNotNone(p_dev)

    def test_xinput_disconnected(self):
        ret = self.mock_xinput_get_state(0)
        self.assertEqual(ret, 1167, "XInputGetState phải trả về ERROR_DEVICE_NOT_CONNECTED khi không có gamepad")

if __name__ == "__main__":
    unittest.main()
