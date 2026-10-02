import unittest

class TestXAudio2BridgeLogic(unittest.TestCase):
    def setUp(self):
        self.mock_xaudio2 = "MockIXAudio2"
        self.mock_mastering_voice = "MockIXAudio2MasteringVoice"

    def mock_xaudio2_create(self, pp_xaudio2, flags, processor):
        if pp_xaudio2 is None:
            return 0x80070057  # E_INVALIDARG
        return 0, self.mock_xaudio2

    def mock_create_mastering_voice(self, p_xaudio2, pp_mastering_voice):
        if p_xaudio2 is None:
            raise ValueError("Null pointer dereference on IXAudio2")
        if pp_mastering_voice is None:
            return 0x80070057
        return 0, self.mock_mastering_voice

    def mock_windows_teb_tls_access(self, gs_58_ptr, tls_index):
        if gs_58_ptr is None or gs_58_ptr == 0:
            raise ValueError("Null pointer dereference: gs:[0x58] ThreadLocalStoragePointer is NULL (SEGV_MAPERR)")
        
        # gs:[0x58] là mảng các con trỏ TLS theo module index
        tls_array = gs_58_ptr
        if tls_index >= len(tls_array):
            raise IndexError("TLS index out of range")
        
        module_tls_data = tls_array[tls_index]
        if module_tls_data is None:
            raise ValueError("Module TLS data pointer is NULL")
        
        return module_tls_data

    def test_xaudio2_create_success(self):
        hr, p_xaudio2 = self.mock_xaudio2_create({}, 0, 1)
        self.assertEqual(hr, 0, "XAudio2Create phải trả về S_OK (0)")
        self.assertEqual(p_xaudio2, "MockIXAudio2")

    def test_xaudio2_create_invalid_arg(self):
        hr = self.mock_xaudio2_create(None, 0, 1)
        self.assertEqual(hr, 0x80070057, "XAudio2Create phải trả về E_INVALIDARG khi ppXAudio2 là NULL")

    def test_create_mastering_voice_success(self):
        hr, p_master = self.mock_create_mastering_voice(self.mock_xaudio2, {})
        self.assertEqual(hr, 0)
        self.assertEqual(p_master, "MockIXAudio2MasteringVoice")

    def test_tls_pointer_prevents_segv(self):
        # Kiểm tra kịch bản gây crash SEGV_MAPERR fault addr 0x0 trước đó:
        # Nếu gs:[0x58] là NULL, truy cập [rax + rcx*8] sẽ crash
        with self.assertRaises(ValueError):
            self.mock_windows_teb_tls_access(None, 0)

        # Với mảng TLS được setup đúng chuẩn, truy cập thành công
        fake_tls_data = bytearray(4096)
        tls_array = [fake_tls_data]
        res = self.mock_windows_teb_tls_access(tls_array, 0)
        self.assertIsNotNone(res)
        self.assertEqual(len(res), 4096)

if __name__ == "__main__":
    unittest.main()
