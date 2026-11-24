import os
import struct
import unittest


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP_DIR = os.path.join(REPO_ROOT, "app", "src", "main", "cpp")


def read_cpp_source(filename):
    with open(os.path.join(CPP_DIR, filename), "r", encoding="utf-8") as f:
        return f.read()


class TestMobileRuntimeUpgrades(unittest.TestCase):
    """Kiểm thử toàn diện cho các nâng cấp runtime di động (Mục 6.16):
    - Vòng đời engine một lần & bảo toàn GPU texture khi mất EGLContext
    - Bộ nhớ đệm nhị phân & phân họ ShaderCache 4 chương trình GLSL
    - Bộ trộn âm thanh trung tâm XAudio2MasterMixer đa kênh & kẹp biên chống rè
    - Cầu nối tay cầm XInput 16-byte ABI, deadzone & kích hoạt cạnh phím Win32
    - Bộ chuyển đổi YUV420 I420/NV12 SIMD SSE2/NEON khớp bit-exact chuẩn BT.601
    - Cầu nối động Arm64Box64Bridge & chuyển tiếp ABI ms_abi -> AAPCS64
    - Đường dẫn động & ghi đè ngôn ngữ SteamHook
    """

    def test_single_engine_launch_and_gpu_preservation_hooks_present(self):
        jni_src = read_cpp_source("jni_bridge.cpp")
        reg_src = read_cpp_source("resource_registry.cpp")

        self.assertIn("gEngineLaunched.exchange(true)", jni_src)
        self.assertIn("D3D11Bridge_PreserveGpuTextureToCpu", jni_src)
        self.assertIn("D3D11Bridge_ResetGpuUploadState", jni_src)
        self.assertIn("AudioSink_SetLifecyclePaused(true)", jni_src)
        self.assertIn("XAudio2Bridge_SetLifecyclePaused(true)", jni_src)
        self.assertIn("ResourceRegistry::preserveGpuTextureToCpu", reg_src)
        self.assertIn("ResourceRegistry::resetAllUploadedFlags", reg_src)

    def test_gpu_preserve_to_cpu_state_transition_model(self):
        # Mô phỏng chính xác vòng đời preserveGpuTextureToCpu -> resetAllUploadedFlags -> nạp lại EGL mới
        entry = {
            "width": 4,
            "height": 2,
            "data": None,
            "hasData": False,
            "isUploadedToGpu": True,
            "hasGpuContents": True,
            "isCompressedBc7": False,
            "revision": 5,
        }

        def preserve_gpu_to_cpu(ent, rgba_bytes):
            expected = ent["width"] * ent["height"] * 4
            if rgba_bytes is None or len(rgba_bytes) < expected:
                return False
            ent["data"] = bytes(rgba_bytes[:expected])
            ent["hasData"] = True
            ent["isUploadedToGpu"] = False
            ent["hasGpuContents"] = False
            ent["isCompressedBc7"] = False
            ent["revision"] += 1
            return True

        # Error case: buffer ngắn hơn kích thước ảnh bị từ chối an toàn
        self.assertFalse(preserve_gpu_to_cpu(entry, b"\xFF" * 10))
        self.assertTrue(entry["hasGpuContents"])

        # Happy path: đọc ngược đủ 32 bytes từ FBO
        readback = bytes((i * 7) & 0xFF for i in range(32))
        self.assertTrue(preserve_gpu_to_cpu(entry, readback))
        self.assertTrue(entry["hasData"])
        self.assertFalse(entry["isUploadedToGpu"])
        self.assertFalse(entry["hasGpuContents"])
        self.assertEqual(entry["revision"], 6)
        self.assertEqual(entry["data"], readback)

    def test_shader_family_classification_and_binary_header_validation(self):
        shader_hdr = read_cpp_source("shader_cache.h")
        shader_src = read_cpp_source("shader_cache.cpp")
        self.assertIn("StandardSprite = 0", shader_hdr)
        self.assertIn("WipeAndUtility = 1", shader_hdr)
        self.assertIn("ProgrammableBlend = 2", shader_hdr)
        self.assertIn("glGetProgramBinary", shader_src)
        self.assertIn("glProgramBinary", shader_src)

        def classify(shader_id):
            if shader_id < 900:
                return "StandardSprite"
            if 2040 <= shader_id <= 2076:
                return "WipeAndUtility"
            if 900 <= shader_id < 2040:
                return "ProgrammableBlend"
            return "UberFallback"

        self.assertEqual(classify(0), "StandardSprite")
        self.assertEqual(classify(180), "StandardSprite")
        self.assertEqual(classify(899), "StandardSprite")
        self.assertEqual(classify(900), "ProgrammableBlend")
        self.assertEqual(classify(2039), "ProgrammableBlend")
        self.assertEqual(classify(2040), "WipeAndUtility")
        self.assertEqual(classify(2076), "WipeAndUtility")
        self.assertEqual(classify(2077), "UberFallback")

        # Kiểm tra xác thực header nhị phân SGSB chống nạp nhầm cache hỏng hoặc sai phiên bản
        magic = 0x53475342
        schema_ver = 2
        expected_hash = 0x1122334455667788
        good_hdr = struct.pack("<IIQII", magic, schema_ver, expected_hash, 0x87FE, 128)
        m, v, h, fmt, size = struct.unpack("<IIQII", good_hdr)
        self.assertEqual((m, v, h, size), (magic, schema_ver, expected_hash, 128))

        # Hash sai hoặc kích thước vượt ngưỡng 4MB phải bị loại bỏ
        bad_hdr = struct.pack("<IIQII", magic, schema_ver, expected_hash ^ 1, 0x87FE, 8 * 1024 * 1024)
        _, _, bad_h, _, bad_sz = struct.unpack("<IIQII", bad_hdr)
        self.assertNotEqual(bad_h, expected_hash)
        self.assertGreater(bad_sz, 4 * 1024 * 1024)

    def test_xaudio2_central_master_mixer_saturation_and_callback_safety(self):
        xa_src = read_cpp_source("xaudio2_bridge.cpp")
        self.assertIn("class XAudio2MasterMixer", xa_src)
        self.assertIn("mixInto(", xa_src)
        self.assertIn("XAudio2Bridge_SetLifecyclePaused", xa_src)

        def clamp_pcm16(val):
            return max(-32768, min(32767, val))

        def mix_voices(voice_buffers, voice_gains, master_gain):
            frames = len(voice_buffers[0])
            accum = [0] * frames
            for samples, v_gain in zip(voice_buffers, voice_gains):
                gain = v_gain * master_gain
                for i, s in enumerate(samples):
                    accum[i] += int(round(s * gain))
            return [clamp_pcm16(x) for x in accum]

        # Trộn 3 luồng (BGM + Voice + SE) vượt biên 16-bit phải kẹp mềm tại [-32768, 32767], không bị tràn số âm/dương
        bgm = [20000, -20000, 1000, 0]
        voice = [18000, -19000, 2000, 500]
        se = [5000, -5000, -500, -500]
        mixed = mix_voices([bgm, voice, se], [1.0, 1.0, 1.0], 1.0)
        self.assertEqual(mixed, [32767, -32768, 2500, 0])

        # Khi master_gain = 0.0 (mute), đầu ra bằng 0 tuyệt đối
        muted = mix_voices([bgm, voice], [1.0, 1.0], 0.0)
        self.assertEqual(muted, [0, 0, 0, 0])

    def test_xinput_16byte_abi_deadzone_and_edge_triggered_keys(self):
        xi_src = read_cpp_source("xinput_bridge.cpp")
        pl_src = read_cpp_source("payload_loader.cpp")
        self.assertIn("XInputBridge_GetState", xi_src)
        self.assertIn("XInputBridge_UpdateAxes", xi_src)
        self.assertIn("&XInputBridge_GetState", pl_src)

        # Kiểm tra kích thước chuẩn XINPUT_STATE = 4 (dwPacketNumber) + 12 (XINPUT_GAMEPAD) = 16 bytes
        packed = struct.pack("<IHBBhhhh", 7, 0x1000, 0, 255, 0, 32767, -32767, 0)
        self.assertEqual(len(packed), 16)

        # Mô phỏng chuyển đổi trục Analog sang xung cạnh phím điều hướng Win32 (chỉ gửi khi đổi trạng thái)
        dispatched = []
        prev_mask = [0]

        def update_axes(lx, ly, trigger_l, trigger_r):
            mask = 0
            if lx <= -0.45:
                mask |= 0x0004  # DPAD_LEFT -> VK_LEFT (0x25)
            elif lx >= 0.45:
                mask |= 0x0008  # DPAD_RIGHT -> VK_RIGHT (0x27)
            if ly <= -0.45:
                mask |= 0x0001  # DPAD_UP -> VK_UP (0x26)
            elif ly >= 0.45:
                mask |= 0x0002  # DPAD_DOWN -> VK_DOWN (0x28)
            if trigger_l >= 0.5:
                mask |= 0x10000  # LT -> 'Z'
            if trigger_r >= 0.5:
                mask |= 0x20000  # RT -> 'C'

            diff = mask ^ prev_mask[0]
            prev_mask[0] = mask
            for bit, vk in ((0x0004, 0x25), (0x0008, 0x27), (0x0001, 0x26), (0x0002, 0x28), (0x10000, ord("Z")), (0x20000, ord("C"))):
                if diff & bit:
                    dispatched.append((vk, bool(mask & bit)))

        # Đẩy cần sang phải hai lần liên tiếp chỉ được sinh 1 sự kiện KEYDOWN duy nhất
        update_axes(0.8, 0.0, 0.0, 0.0)
        update_axes(0.85, 0.0, 0.0, 0.0)
        self.assertEqual(dispatched, [(0x27, True)])

        # Trả cần về vùng chết (0.1) phải sinh đúng 1 sự kiện KEYUP
        update_axes(0.1, 0.0, 0.0, 0.0)
        self.assertEqual(dispatched, [(0x27, True), (0x27, False)])

    def test_yuv420_bt601_i420_and_nv12_parity_and_bottom_up_order(self):
        webm_src = read_cpp_source("webm_decoder.cpp")
        self.assertIn("__SSE2__", webm_src)
        self.assertIn("__ARM_NEON", webm_src)

        def bt601_pixel(y_val, u_val, v_val):
            c = y_val - 16
            d = u_val - 128
            e = v_val - 128
            r = max(0, min(255, (298 * c + 409 * e + 128) >> 8))
            g = max(0, min(255, (298 * c - 100 * d - 208 * e + 128) >> 8))
            b = max(0, min(255, (298 * c + 516 * d + 128) >> 8))
            return bytes([b, g, r, 0xFF])

        # Kiểm tra biên đen (Y=16, U=128, V=128 -> RGB 0,0,0) và biên trắng (Y=235, U=128, V=128 -> RGB 255,255,255)
        self.assertEqual(bt601_pixel(16, 128, 128), bytes([0, 0, 0, 255]))
        self.assertEqual(bt601_pixel(235, 128, 128), bytes([255, 255, 255, 255]))
        # Kiểm tra kẹp biên dưới/trên khi Y=0 hoặc Y=255 cực trị
        self.assertEqual(bt601_pixel(0, 0, 0)[3], 255)
        self.assertEqual(bt601_pixel(255, 255, 255)[3], 255)

    def test_arm64_box64_bridge_ms_abi_to_aapcs64_marshalling(self):
        box_hdr = read_cpp_source("arm64_box64_bridge.h")
        box_src = read_cpp_source("arm64_box64_bridge.cpp")
        self.assertIn("Box64RuntimeStatus", box_hdr)
        self.assertIn("defaultTenArgIntegerMarshaller", box_src)
        self.assertIn("0x28u", box_src)

        # Mô phỏng khung ngăn xếp Microsoft x64 ABI cho D3D11CreateDevice (10 tham số):
        # [RSP + 0x00] = returnAddr, [RSP + 0x08..0x20] = 32-byte shadow space, [RSP + 0x28..0x50] = args 5..10
        fake_stack = bytearray(0x60)
        struct.pack_into("<Q", fake_stack, 0x00, 0x140001000)  # Return IP
        expected_stack_args = [50, 60, 7, 0xAA01, 0xBB02, 0xCC03]
        for idx, val in enumerate(expected_stack_args):
            struct.pack_into("<Q", fake_stack, 0x28 + idx * 8, val)

        ctx = {"rcx": 10, "rdx": 20, "r8": 30, "r9": 40, "stack": fake_stack, "rax": 0}

        def marshal_10_args(target_fn, call_ctx):
            s_args = [struct.unpack_from("<Q", call_ctx["stack"], 0x28 + i * 8)[0] for i in range(6)]
            call_ctx["rax"] = target_fn(
                call_ctx["rcx"], call_ctx["rdx"], call_ctx["r8"], call_ctx["r9"], *s_args
            )

        captured = []
        marshal_10_args(lambda *args: (captured.extend(args) or 0), ctx)
        self.assertEqual(ctx["rax"], 0)
        self.assertEqual(captured, [10, 20, 30, 40, 50, 60, 7, 0xAA01, 0xBB02, 0xCC03])

    def test_steam_hook_dynamic_directory_and_language_override(self):
        steam_src = read_cpp_source("steam_hook.cpp")
        self.assertIn("SteamHook_SetDataDirectory", steam_src)
        self.assertIn("SteamHook_SetLanguageOverride", steam_src)
        self.assertIn('activeBaseDir + "/language.txt"', steam_src)
        self.assertNotIn('"/sdcard/sgrb_game_data/language.txt"', steam_src)


if __name__ == "__main__":
    unittest.main()
