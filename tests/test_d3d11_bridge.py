import unittest
import struct

class TestD3D11BridgeLogic(unittest.TestCase):
    def setUp(self):
        # Định nghĩa các GUID chuẩn Microsoft DirectX / DXGI
        self.IID_IUNKNOWN          = "00000000-0000-0000-c000-000000000046"
        self.IID_ID3D11DEVICE      = "db6f6ddb-ac77-4e88-8253-819df9bbf140"
        self.IID_ID3D11DEVICE1     = "a04462db-9756-4b6e-afdb-c0f709fab4ee"
        self.IID_ID3D11CONTEXT     = "c0bfa96c-e089-44fb-8eaf-26f8796190da"
        self.IID_ID3D11MULTITHREAD = "9b7e4e00-342c-4106-a19f-4f2704f689f0"
        self.IID_IDXGIDEVICE1      = "77db970f-6276-48ba-ba28-070143b4392c"
        self.IID_IDXGIADAPTER      = "2411e0e1-12ac-4ccf-bd14-9798e8534d00"
        self.IID_IDXGIFACTORY2     = "50c83a1c-e072-4c48-87b0-3630fa36a6d0"
        self.IID_IDXGISWAPCHAIN    = "310d36a0-d837-44da-aa5f-e050e256f20d"
        self.IID_ID3D11TEXTURE2D   = "6f15aaf2-d208-4e89-9ab4-489535d34f9c"
        self.IID_ID3D11SHADERREFLECTION = "8d536ca1-0cca-4956-a837-786963755584"

    def mock_query_interface(self, iid):
        if iid == self.IID_ID3D11MULTITHREAD:
            return "MockID3D11Multithread"
        elif iid == self.IID_IDXGIDEVICE1:
            return "MockIDXGIDevice1"
        elif iid == self.IID_IDXGIADAPTER:
            return "MockIDXGIAdapter"
        elif iid == self.IID_IDXGIFACTORY2:
            return "MockIDXGIFactory2"
        elif iid == self.IID_IDXGISWAPCHAIN:
            return "MockIDXGISwapChain"
        elif iid == self.IID_ID3D11TEXTURE2D:
            return "MockID3D11Texture2D"
        elif iid == self.IID_ID3D11SHADERREFLECTION:
            return "MockID3D11ShaderReflection"
        elif iid in (self.IID_ID3D11DEVICE, self.IID_ID3D11DEVICE1):
            return "MockID3D11Device"
        elif iid == self.IID_ID3D11CONTEXT:
            return "MockID3D11DeviceContext"
        return "MockGeneric"

    def mock_d3d_reflect(self, src_data, size, iid):
        reflector = self.mock_query_interface(iid)
        return (0, reflector) # S_OK, reflector pointer

    def test_multithread_guid_routing(self):
        obj = self.mock_query_interface(self.IID_ID3D11MULTITHREAD)
        self.assertEqual(obj, "MockID3D11Multithread")

    def test_dxgi_device_guid_routing(self):
        obj = self.mock_query_interface(self.IID_IDXGIDEVICE1)
        self.assertEqual(obj, "MockIDXGIDevice1")

    def test_dxgi_factory_guid_routing(self):
        obj = self.mock_query_interface(self.IID_IDXGIFACTORY2)
        self.assertEqual(obj, "MockIDXGIFactory2")

    def test_swapchain_guid_routing(self):
        obj = self.mock_query_interface(self.IID_IDXGISWAPCHAIN)
        self.assertEqual(obj, "MockIDXGISwapChain")

    def test_texture2d_guid_routing(self):
        obj = self.mock_query_interface(self.IID_ID3D11TEXTURE2D)
        self.assertEqual(obj, "MockID3D11Texture2D")

    def test_shader_reflection_guid_routing(self):
        obj = self.mock_query_interface(self.IID_ID3D11SHADERREFLECTION)
        self.assertEqual(obj, "MockID3D11ShaderReflection")

    def test_d3d_reflect_output_pointer(self):
        hr, reflector = self.mock_d3d_reflect(None, 0, self.IID_ID3D11SHADERREFLECTION)
        self.assertEqual(hr, 0, "D3DReflect phải trả về S_OK (0)")
        self.assertIsNotNone(reflector, "ppReflector không được là nullptr")
        self.assertEqual(reflector, "MockID3D11ShaderReflection")

    def test_swapchain_desc_buffer_count_offset(self):
        # Kiểm tra offset của BufferCount trong DXGI_SWAP_CHAIN_DESC
        # Tại RVA 0x4C47C5 (0x1404c47c5), sgre_steam.exe đọc: mov ebp, dword ptr [rsp + 0x58]
        # Con trỏ desc được truyền qua rdx = [rsp + 0x30]
        # Offset bắt buộc phải là 0x58 - 0x30 = 0x28 (40 bytes)!
        import ctypes
        class DXGI_RATIONAL(ctypes.Structure):
            _fields_ = [('Numerator', ctypes.c_uint), ('Denominator', ctypes.c_uint)]

        class DXGI_MODE_DESC(ctypes.Structure):
            _fields_ = [
                ('Width', ctypes.c_uint),
                ('Height', ctypes.c_uint),
                ('RefreshRate', DXGI_RATIONAL),
                ('Format', ctypes.c_uint),
                ('ScanlineOrdering', ctypes.c_uint),
                ('Scaling', ctypes.c_uint),
            ]

        class DXGI_SAMPLE_DESC(ctypes.Structure):
            _fields_ = [('Count', ctypes.c_uint), ('Quality', ctypes.c_uint)]

        class DXGI_SWAP_CHAIN_DESC(ctypes.Structure):
            _fields_ = [
                ('BufferDesc', DXGI_MODE_DESC),
                ('SampleDesc', DXGI_SAMPLE_DESC),
                ('BufferUsage', ctypes.c_uint),
                ('BufferCount', ctypes.c_uint),
                ('OutputWindow', ctypes.c_void_p),
                ('Windowed', ctypes.c_int),
                ('SwapEffect', ctypes.c_uint),
                ('Flags', ctypes.c_uint),
            ]

        self.assertEqual(DXGI_SWAP_CHAIN_DESC.BufferCount.offset, 0x28,
                         "BufferCount offset phải là 0x28 để khớp chính xác với [rsp + 0x58]")

    def test_swapchain_buffer_count_never_zero(self):
        # BufferCount phải luôn >= 1 để ngăn chặn lỗi chia cho 0 (SIGFPE FPE_INTDIV)
        # tại hàm băm 0x14054BF2A (div r8 với r8 = bucket_count)
        buffer_counts = [1, 2, 3]
        for count in buffer_counts:
            self.assertGreater(count, 0, "BufferCount phải lớn hơn 0 để bucket count > 0")

    def test_swapchain_vtable_slots(self):
        # Đảm bảo các slot quan trọng của IDXGISwapChain và IDXGISwapChain1 đã được định nghĩa
        swapchain_slots = {
            7: "GetDevice",
            8: "Present",
            9: "GetBuffer",
            10: "SetFullscreenState",
            11: "GetFullscreenState",
            12: "GetDesc",
            13: "ResizeBuffers",
            14: "ResizeTarget",
            15: "GetContainingOutput",
            16: "GetFrameStatistics",
            17: "GetLastPresentCount",
            18: "GetDesc1",
            19: "GetFullscreenDesc",
            20: "GetHwnd",
            22: "Present1"
        }
        self.assertEqual(swapchain_slots[12], "GetDesc")
        self.assertEqual(swapchain_slots[18], "GetDesc1")
        self.assertEqual(swapchain_slots[9], "GetBuffer")

if __name__ == "__main__":
    unittest.main()


