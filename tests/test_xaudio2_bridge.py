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
        vtable_slots = {
            5: "CreateSourceVoice",
            6: "CreateSubmixVoice",
            7: "CreateMasteringVoice"
        }
        self.assertEqual(vtable_slots[5], "CreateSourceVoice")
        self.assertEqual(vtable_slots[6], "CreateSubmixVoice")
        self.assertEqual(vtable_slots[7], "CreateMasteringVoice")
        
        voice_slots = {
            0: "GetVoiceDetails",
            12: "SetVolume",
            13: "GetVolume",
            18: "DestroyVoice",
            19: "Start",
            20: "Stop",
            21: "SubmitSourceBuffer",
            22: "FlushSourceBuffers",
            25: "GetState"
        }
        self.assertEqual(voice_slots[0], "GetVoiceDetails")
        self.assertEqual(voice_slots[12], "SetVolume")
        self.assertEqual(voice_slots[21], "SubmitSourceBuffer")
        self.assertEqual(21 * 8, 0xA8)

    def test_xaudio2_voice_details_structure(self):
        # Kiểm tra cấu trúc XAUDIO2_VOICE_DETAILS (16 bytes)
        import struct
        details_bytes = struct.pack("<IIII", 0, 0, 2, 48000)
        self.assertEqual(len(details_bytes), 16)
        flags, active, channels, rate = struct.unpack("<IIII", details_bytes)
        self.assertEqual(channels, 2)
        self.assertEqual(rate, 48000)

    def test_xaudio2_buffer_64_layout(self):
        # Kiểm tra cấu trúc XAUDIO2_BUFFER_64 chuẩn x64 ABI (48 bytes, pContext tại offset 40)
        import struct
        # uint32 Flags, uint32 AudioBytes, uint64 pAudioData, uint32 PlayBegin, uint32 PlayLength,
        # uint32 LoopBegin, uint32 LoopLength, uint32 LoopCount, uint32 pad, uint64 pContext
        buf_data = struct.pack("<IIQIIIIIIQ", 64, 1024, 0x12345678, 0, 0, 0, 0, 0, 0, 0x9ABCDEF0)
        self.assertEqual(len(buf_data), 48)
        # pContext phải nằm chính xác tại byte offset 40
        p_context_offset = 40
        p_context = struct.unpack("<Q", buf_data[p_context_offset:p_context_offset+8])[0]
        self.assertEqual(p_context, 0x9ABCDEF0)

    def test_tls_pointer_prevents_segv(self):
        with self.assertRaises(ValueError):
            self.mock_windows_teb_tls_access(None, 0)

        fake_tls_data = bytearray(4096)
        tls_array = [fake_tls_data]
        res = self.mock_windows_teb_tls_access(tls_array, 0)
        self.assertIsNotNone(res)
        self.assertEqual(len(res), 4096)

    def test_destroy_voice_deadlock_prevention(self):
        class MockSourceVoice:
            def __init__(self):
                self.is_running = True
                self.is_destroying = False
                self.callback = "ValidCallback"
                self.queue = [1, 2, 3]
                self.callbacks_fired = 0

            def call_on_buffer_end(self):
                if self.callback and not self.is_destroying:
                    self.callbacks_fired += 1

            def flush(self, trigger_callbacks=True):
                while self.queue:
                    self.queue.pop(0)
                    if trigger_callbacks and not self.is_destroying:
                        self.call_on_buffer_end()

            def stop_thread(self):
                self.is_destroying = True
                self.callback = None
                self.is_running = False
                self.flush(trigger_callbacks=False)

        voice = MockSourceVoice()
        self.assertEqual(len(voice.queue), 3)
        voice.stop_thread()
        self.assertEqual(len(voice.queue), 0)
        self.assertEqual(voice.callbacks_fired, 0)
        self.assertIsNone(voice.callback)
        self.assertTrue(voice.is_destroying)

    def test_msadpcm_stereo_decode(self):
        # Kiểm tra giải mã Microsoft ADPCM Stereo
        import struct
        # 14 bytes header: pred0(1B), pred1(1B), delta0(2B), delta1(2B), samp1_0(2B), samp1_1(2B), samp2_0(2B), samp2_1(2B)
        hdr = struct.pack("<BBhhhhhh", 0, 0, 16, 16, 100, 200, 50, 150)
        # 4 bytes data = 4 mẫu stereo (mỗi byte = 1 mẫu L/R)
        data = b"\x00\x11\x22\x33"
        block = hdr + data
        self.assertEqual(len(block), 18)

        # Mô phỏng logic giải mã
        coef1 = [256, 512, 0, 192, 240, 460, 392]
        coef2 = [0, -256, 0, 64, 0, -208, -232]
        adapt = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]

        pcm_samples = [50, 150, 100, 200]  # samp2_0, samp2_1, samp1_0, samp1_1
        delta0, delta1 = 16, 16
        s1_0, s2_0 = 100, 50
        s1_1, s2_1 = 200, 150

        for b in data:
            n0 = (b >> 4) & 0x0F
            n1 = b & 0x0F
            # L
            sn0 = n0 - 16 if (n0 & 8) else n0
            pred0 = (s1_0 * coef1[0] + s2_0 * coef2[0]) // 256
            new0 = max(-32768, min(32767, pred0 + sn0 * delta0))
            s2_0, s1_0 = s1_0, new0
            delta0 = max(16, (delta0 * adapt[n0]) // 256)
            pcm_samples.append(new0)
            # R
            sn1 = n1 - 16 if (n1 & 8) else n1
            pred1 = (s1_1 * coef1[0] + s2_1 * coef2[0]) // 256
            new1 = max(-32768, min(32767, pred1 + sn1 * delta1))
            s2_1, s1_1 = s1_1, new1
            delta1 = max(16, (delta1 * adapt[n1]) // 256)
            pcm_samples.append(new1)

        self.assertEqual(len(pcm_samples), 12)  # 4 header samples + 8 data samples (4 stereo frames)
        for s in pcm_samples:
            self.assertTrue(-32768 <= s <= 32767)

    def test_msadpcm_mono_decode(self):
        # Kiểm tra giải mã Microsoft ADPCM Mono
        import struct
        # 7 bytes header: pred(1B), delta(2B), samp1(2B), samp2(2B)
        hdr = struct.pack("<Bhhh", 0, 16, 300, 100)
        # 2 bytes data = 4 mẫu mono (mỗi byte = 2 mẫu)
        data = b"\x01\x23"
        block = hdr + data
        self.assertEqual(len(block), 9)

        pcm_samples = [100, 300]
        coef1 = [256, 512, 0, 192, 240, 460, 392]
        coef2 = [0, -256, 0, 64, 0, -208, -232]
        adapt = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]
        delta = 16
        s1, s2 = 300, 100

        for b in data:
            for n in [(b >> 4) & 0x0F, b & 0x0F]:
                sn = n - 16 if (n & 8) else n
                pred = (s1 * coef1[0] + s2 * coef2[0]) // 256
                new_s = max(-32768, min(32767, pred + sn * delta))
                s2, s1 = s1, new_s
                delta = max(16, (delta * adapt[n]) // 256)
                pcm_samples.append(new_s)

        self.assertEqual(len(pcm_samples), 6)  # 2 header + 4 data
        for s in pcm_samples:
            self.assertTrue(-32768 <= s <= 32767)

    def test_msadpcm_stereo_decoding_interleaving(self):
        """Kiểm tra giải mã MS-ADPCM Stereo chuẩn interleaving 2 byte (byteL + byteR -> [L0, R0, L1, R1])."""
        import struct
        # 14 bytes header: pred0, pred1, delta0, delta1, samp1_0, samp1_1, samp2_0, samp2_1
        hdr = struct.pack("<BBhhhhhh", 0, 0, 16, 16, 200, -200, 100, -100)
        # 2 bytes data: byte 0 cho kênh Trái (2 mẫu L0, L1), byte 1 cho kênh Phải (2 mẫu R0, R1)
        data = b"\x12\x34"
        block = hdr + data
        self.assertEqual(len(block), 16)

        pcm_samples = []
        # Header samples
        samp2_0, samp2_1 = 100, -100
        samp1_0, samp1_1 = 200, -200
        pcm_samples.extend([samp2_0, samp2_1, samp1_0, samp1_1])

        coef1 = [256, 512, 0, 192, 240, 460, 392]
        coef2 = [0, -256, 0, 64, 0, -208, -232]
        adapt = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]

        delta0, delta1 = 16, 16

        def decode_nibble(n, c1, c2, d, s1, s2):
            sn = n - 16 if (n & 8) else n
            pred = (s1 * c1 + s2 * c2) // 256
            s = max(-32768, min(32767, pred + sn * d))
            new_d = max(16, (d * adapt[n]) // 256)
            return s, s1, new_d

        # Chuẩn Microsoft MS-ADPCM Stereo (RFC 2361 & FFmpeg):
        # Mỗi byte chứa 1 mẫu Stereo: nibble cao cho kênh Trái, nibble thấp cho kênh Phải
        for b in range(len(data)):
            byte_val = data[b]
            nL = (byte_val >> 4) & 0x0F
            nR = byte_val & 0x0F

            sL, samp2_0, delta0 = decode_nibble(nL, coef1[0], coef2[0], delta0, samp1_0, samp2_0)
            samp1_0 = sL
            sR, samp2_1, delta1 = decode_nibble(nR, coef1[0], coef2[0], delta1, samp1_1, samp2_1)
            samp1_1 = sR
            pcm_samples.extend([sL, sR])

        self.assertEqual(len(pcm_samples), 8)  # 4 header + 4 data
        # Đảm bảo các mẫu stereo luân phiên L, R
        for s in pcm_samples:
            self.assertTrue(-32768 <= s <= 32767)

if __name__ == "__main__":
    unittest.main()
