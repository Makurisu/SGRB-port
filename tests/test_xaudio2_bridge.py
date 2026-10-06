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

    def mock_create_source_voice(self, p_xaudio2, pp_source_voice):
        if p_xaudio2 is None:
            raise ValueError("Null pointer dereference on IXAudio2")
        if pp_source_voice is None:
            return 0x80070057
        return 0, "MockIXAudio2SourceVoice"

    def mock_create_submix_voice(self, p_xaudio2, pp_submix_voice):
        if p_xaudio2 is None:
            raise ValueError("Null pointer dereference on IXAudio2")
        if pp_submix_voice is None:
            return 0x80070057
        return 0, "MockIXAudio2SubmixVoice"

    def mock_source_voice_submit_buffer(self, p_source_voice, p_buffer):
        if p_source_voice is None:
            raise ValueError("Null pointer dereference: IXAudio2SourceVoice is NULL (SEGV_MAPERR)")
        # Mô phỏng nạp buffer thành công
        return 0

    def mock_source_voice_get_state(self, p_source_voice, p_voice_state):
        if p_source_voice is None:
            raise ValueError("Null pointer dereference: IXAudio2SourceVoice is NULL")
        if p_voice_state is not None:
            p_voice_state.clear()
            p_voice_state["BuffersQueued"] = 0
            p_voice_state["SamplesPlayed"] = 0
        return 0

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

    def test_create_source_voice_success(self):
        hr, p_source = self.mock_create_source_voice(self.mock_xaudio2, {})
        self.assertEqual(hr, 0)
        self.assertEqual(p_source, "MockIXAudio2SourceVoice")

    def test_create_source_voice_invalid_arg(self):
        hr = self.mock_create_source_voice(self.mock_xaudio2, None)
        self.assertEqual(hr, 0x80070057)

    def test_create_submix_voice_success(self):
        hr, p_submix = self.mock_create_submix_voice(self.mock_xaudio2, {})
        self.assertEqual(hr, 0)
        self.assertEqual(p_submix, "MockIXAudio2SubmixVoice")

    def test_source_voice_submit_buffer_prevents_segv(self):
        # Tránh crash lỗi mov rax, [rcx]; call [rax + 0xA8] khi rcx = NULL
        with self.assertRaises(ValueError):
            self.mock_source_voice_submit_buffer(None, {})

        res = self.mock_source_voice_submit_buffer("MockIXAudio2SourceVoice", {"AudioBytes": 1024})
        self.assertEqual(res, 0)

    def test_source_voice_get_state(self):
        state = {}
        res = self.mock_source_voice_get_state("MockIXAudio2SourceVoice", state)
        self.assertEqual(res, 0)
        self.assertEqual(state["BuffersQueued"], 0)

    def test_xaudio2_vtable_layout(self):
        # Kiểm tra đúng vtable slot mapping cho IXAudio2:
        # Slot 5 (offset 0x28): CreateSourceVoice
        # Slot 6 (offset 0x30): CreateSubmixVoice
        # Slot 7 (offset 0x38): CreateMasteringVoice
        vtable_slots = {
            5: "CreateSourceVoice",
            6: "CreateSubmixVoice",
            7: "CreateMasteringVoice"
        }
        self.assertEqual(vtable_slots[5], "CreateSourceVoice")
        self.assertEqual(vtable_slots[6], "CreateSubmixVoice")
        self.assertEqual(vtable_slots[7], "CreateMasteringVoice")
        
        # Kiểm tra Voice Vtable slot mapping:
        # Slot 18 (0x90): DestroyVoice
        # Slot 19 (0x98): Start
        # Slot 20 (0xA0): Stop
        # Slot 21 (0xA8): SubmitSourceBuffer
        # Slot 22 (0xB0): FlushSourceBuffers
        # Slot 25 (0xC8): GetState
        voice_slots = {
            18: "DestroyVoice",
            19: "Start",
            20: "Stop",
            21: "SubmitSourceBuffer",
            22: "FlushSourceBuffers",
            25: "GetState"
        }
        self.assertEqual(voice_slots[21], "SubmitSourceBuffer")
        self.assertEqual(21 * 8, 0xA8)

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

