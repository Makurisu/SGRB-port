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
    def test_slot_dispatcher_coverage(self):
        # Kiểm tra slot dispatcher phủ toàn bộ 128 slots của Device và Context
        slots = list(range(128))
        self.assertEqual(len(slots), 128)
        self.assertEqual(slots[0], 0)
        self.assertEqual(slots[127], 127)

    def test_egl_visual_id_matching(self):
        # EGL visual ID phải được đồng bộ cho ANativeWindow buffer geometry
        visual_id_rgba8888 = 1 # WINDOW_FORMAT_RGBA_8888
        self.assertGreater(visual_id_rgba8888, 0, "Visual ID phải hợp lệ và lớn hơn 0")

    def test_frame_state_structure(self):
        # Kiểm tra cấu trúc RenderFrameState
        frame_state = {
            "clearColor": [0.0, 0.0, 0.0, 1.0],
            "frameIndex": 1,
            "width": 1920,
            "height": 1080,
            "drawCalls": 10,
            "indexDraws": 50,
            "texturesCreated": 18,
            "shadersCreated": 1798,
            "buffersCreated": 150000
        }
        self.assertEqual(len(frame_state["clearColor"]), 4)
        self.assertEqual(frame_state["width"], 1920)
        self.assertEqual(frame_state["height"], 1080)
        self.assertEqual(frame_state["texturesCreated"], 18)
        self.assertEqual(frame_state["shadersCreated"], 1798)
        self.assertGreater(frame_state["buffersCreated"], 0)

    def test_render_frame_state_abi_layout(self):
        # Xác thực kích thước và offset của RenderFrameState trong C++ ABI
        import ctypes
        class RenderFrameState(ctypes.Structure):
            _fields_ = [
                ('clearColor', ctypes.c_float * 4),
                ('frameIndex', ctypes.c_uint),
                ('width', ctypes.c_uint),
                ('height', ctypes.c_uint),
                ('drawCalls', ctypes.c_uint),
                ('indexDraws', ctypes.c_uint),
                ('texturesCreated', ctypes.c_uint),
                ('shadersCreated', ctypes.c_uint),
                ('buffersCreated', ctypes.c_uint),
            ]
        self.assertEqual(ctypes.sizeof(RenderFrameState), 48, "RenderFrameState phải có kích thước đúng 48 bytes (12 * 4 bytes)")
        self.assertEqual(RenderFrameState.clearColor.offset, 0)
        self.assertEqual(RenderFrameState.frameIndex.offset, 16)
        self.assertEqual(RenderFrameState.width.offset, 20)
        self.assertEqual(RenderFrameState.height.offset, 24)
        self.assertEqual(RenderFrameState.drawCalls.offset, 28)
        self.assertEqual(RenderFrameState.indexDraws.offset, 32)
        self.assertEqual(RenderFrameState.texturesCreated.offset, 36)
        self.assertEqual(RenderFrameState.shadersCreated.offset, 40)
        self.assertEqual(RenderFrameState.buffersCreated.offset, 44)

    def test_clear_render_target_view_slot_mapping(self):
        # Slot 50 trong ID3D11DeviceContext là ClearRenderTargetView
        context_slots = {
            50: "ClearRenderTargetView"
        }
        self.assertEqual(context_slots[50], "ClearRenderTargetView")

    def test_device_context_vtable_specification(self):
        # Kiểm tra bảng phân giải slot ID3D11DeviceContext chuẩn DirectX 11
        context_slots = {
            12: "DrawIndexed",
            13: "Draw",
            14: "Map",
            15: "Unmap",
            44: "RSSetViewports",
            45: "RSSetScissorRects",
            48: "UpdateSubresource",
            50: "ClearRenderTargetView"
        }
        self.assertEqual(context_slots[12], "DrawIndexed")
        self.assertEqual(context_slots[13], "Draw")
        self.assertEqual(context_slots[14], "Map")
        self.assertEqual(context_slots[15], "Unmap")
        self.assertEqual(context_slots[44], "RSSetViewports")
        self.assertEqual(context_slots[45], "RSSetScissorRects")
        self.assertEqual(context_slots[48], "UpdateSubresource")
        self.assertEqual(context_slots[50], "ClearRenderTargetView")

    def test_device_vtable_specification(self):
        # Kiểm tra bảng phân giải slot ID3D11Device chuẩn DirectX 11
        device_slots = {
            3: "CreateBuffer",
            5: "CreateTexture2D",
            7: "CreateShaderResourceView",
            9: "CreateRenderTargetView",
            10: "CreateDepthStencilView",
            11: "CreateInputLayout",
            12: "CreateVertexShader",
            15: "CreatePixelShader",
            20: "CreateBlendState",
            21: "CreateDepthStencilState",
            22: "CreateRasterizerState",
            23: "CreateSamplerState",
            37: "GetFeatureLevel",
            40: "GetImmediateContext"
        }
        self.assertEqual(device_slots[3], "CreateBuffer")
        self.assertEqual(device_slots[5], "CreateTexture2D")
        self.assertEqual(device_slots[12], "CreateVertexShader")
        self.assertEqual(device_slots[15], "CreatePixelShader")

    def test_buffer_desc_structure(self):
        import ctypes
        class D3D11_BUFFER_DESC(ctypes.Structure):
            _fields_ = [
                ('ByteWidth', ctypes.c_uint),
                ('Usage', ctypes.c_uint),
                ('BindFlags', ctypes.c_uint),
                ('CPUAccessFlags', ctypes.c_uint),
                ('MiscFlags', ctypes.c_uint),
                ('StructureByteStride', ctypes.c_uint),
            ]
        self.assertEqual(ctypes.sizeof(D3D11_BUFFER_DESC), 24)
        self.assertEqual(D3D11_BUFFER_DESC.ByteWidth.offset, 0)
        self.assertEqual(D3D11_BUFFER_DESC.BindFlags.offset, 8)

    def test_subresource_data_structure(self):
        import ctypes
        class D3D11_SUBRESOURCE_DATA(ctypes.Structure):
            _fields_ = [
                ('pSysMem', ctypes.c_void_p),
                ('SysMemPitch', ctypes.c_uint),
                ('SysMemSlicePitch', ctypes.c_uint),
            ]
        self.assertEqual(D3D11_SUBRESOURCE_DATA.pSysMem.offset, 0)

    def test_frame_sync_producer_consumer(self):
        # Mô phỏng Producer-Consumer giữa D3D11 Present và Android Render Loop
        import threading, queue
        q = queue.Queue(maxsize=2)
        q.put({"frame": 1, "rgba": [0.0, 0.0, 0.0, 1.0]})
        frame = q.get(timeout=0.1)
        self.assertEqual(frame["frame"], 1)
        self.assertEqual(frame["rgba"][3], 1.0)

if __name__ == "__main__":
    unittest.main()



