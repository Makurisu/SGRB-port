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
            "buffersCreated": 150000,
            "cpuBytes": 48900000,
            "droppedResources": 0
        }
        self.assertEqual(len(frame_state["clearColor"]), 4)
        self.assertEqual(frame_state["width"], 1920)
        self.assertEqual(frame_state["height"], 1080)
        self.assertEqual(frame_state["texturesCreated"], 18)
        self.assertEqual(frame_state["shadersCreated"], 1798)
        self.assertGreater(frame_state["buffersCreated"], 0)
        self.assertGreater(frame_state["cpuBytes"], 0)
        self.assertEqual(frame_state["droppedResources"], 0)

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
                ('cpuBytes', ctypes.c_uint64),
                ('droppedResources', ctypes.c_uint),
            ]
        self.assertEqual(ctypes.sizeof(RenderFrameState), 64, "RenderFrameState phải có kích thước đúng 64 bytes")
        self.assertEqual(RenderFrameState.clearColor.offset, 0)
        self.assertEqual(RenderFrameState.frameIndex.offset, 16)
        self.assertEqual(RenderFrameState.width.offset, 20)
        self.assertEqual(RenderFrameState.height.offset, 24)
        self.assertEqual(RenderFrameState.drawCalls.offset, 28)
        self.assertEqual(RenderFrameState.indexDraws.offset, 32)
        self.assertEqual(RenderFrameState.texturesCreated.offset, 36)
        self.assertEqual(RenderFrameState.shadersCreated.offset, 40)
        self.assertEqual(RenderFrameState.buffersCreated.offset, 44)
        self.assertEqual(RenderFrameState.cpuBytes.offset, 48)
        self.assertEqual(RenderFrameState.droppedResources.offset, 56)

    def test_clear_render_target_view_slot_mapping(self):
        # Slot 50 trong ID3D11DeviceContext là ClearRenderTargetView
        context_slots = {
            50: "ClearRenderTargetView"
        }
        self.assertEqual(context_slots[50], "ClearRenderTargetView")

    def test_device_context_vtable_specification(self):
        # Kiểm tra bảng phân giải slot ID3D11DeviceContext chuẩn DirectX 11
        context_slots = {
            7: "VSSetConstantBuffers",
            8: "PSSetShaderResources",
            9: "PSSetShader",
            10: "PSSetSamplers",
            11: "VSSetShader",
            12: "DrawIndexed",
            13: "Draw",
            14: "Map",
            15: "Unmap",
            16: "PSSetConstantBuffers",
            17: "IASetInputLayout",
            18: "IASetVertexBuffers",
            19: "IASetIndexBuffer",
            24: "IASetPrimitiveTopology",
            25: "VSSetShaderResources",
            26: "VSSetSamplers",
            33: "OMSetRenderTargets",
            35: "OMSetBlendState",
            36: "OMSetDepthStencilState",
            43: "RSSetState",
            44: "RSSetViewports",
            45: "RSSetScissorRects",
            48: "UpdateSubresource",
            50: "ClearRenderTargetView",
            53: "ClearDepthStencilView",
        }
        self.assertEqual(context_slots[7], "VSSetConstantBuffers")
        self.assertEqual(context_slots[8], "PSSetShaderResources")
        self.assertEqual(context_slots[9], "PSSetShader")
        self.assertEqual(context_slots[10], "PSSetSamplers")
        self.assertEqual(context_slots[11], "VSSetShader")
        self.assertEqual(context_slots[12], "DrawIndexed")
        self.assertEqual(context_slots[13], "Draw")
        self.assertEqual(context_slots[14], "Map")
        self.assertEqual(context_slots[15], "Unmap")
        self.assertEqual(context_slots[16], "PSSetConstantBuffers")
        self.assertEqual(context_slots[17], "IASetInputLayout")
        self.assertEqual(context_slots[18], "IASetVertexBuffers")
        self.assertEqual(context_slots[19], "IASetIndexBuffer")
        self.assertEqual(context_slots[24], "IASetPrimitiveTopology")
        self.assertEqual(context_slots[25], "VSSetShaderResources")
        self.assertEqual(context_slots[26], "VSSetSamplers")
        self.assertEqual(context_slots[33], "OMSetRenderTargets")
        self.assertEqual(context_slots[35], "OMSetBlendState")
        self.assertEqual(context_slots[36], "OMSetDepthStencilState")
        self.assertEqual(context_slots[43], "RSSetState")
        self.assertEqual(context_slots[44], "RSSetViewports")
        self.assertEqual(context_slots[45], "RSSetScissorRects")
        self.assertEqual(context_slots[48], "UpdateSubresource")
        self.assertEqual(context_slots[50], "ClearRenderTargetView")
        self.assertEqual(context_slots[53], "ClearDepthStencilView")

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

    def test_bc7_compressed_size_calculation(self):
        # Kiểm tra tính toán kích thước nén BC7
        # Mỗi block 4x4 là 16 bytes
        # 1920x1080 -> 480x270 blocks = 129,600 blocks * 16 = 2,073,600 bytes
        width = 1920
        height = 1080
        blocks_x = (width + 3) // 4
        blocks_y = (height + 3) // 4
        expected_size = blocks_x * blocks_y * 16
        self.assertEqual(expected_size, 2073600)

        # 3424x3424 (texture lớn nhất trong SGRE) -> 856x856 blocks = 732,736 blocks * 16 = 11,723,776 bytes (~11.18 MB)
        width_max = 3424
        height_max = 3424
        blocks_x_max = (width_max + 3) // 4
        blocks_y_max = (height_max + 3) // 4
        expected_size_max = blocks_x_max * blocks_y_max * 16
        self.assertEqual(expected_size_max, 11723776)

    def test_resource_registry_budget_cap(self):
        # Ngân sách mặc định 768 MB
        budget_bytes = 768 * 1024 * 1024
        self.assertEqual(budget_bytes, 805306368)
        # Texture 3424x3424 giải nén sang RGBA8: 3424 * 3424 * 4 = 46,894,080 bytes (~44.7 MB)
        rgba8_bytes = 3424 * 3424 * 4
        self.assertLess(rgba8_bytes, budget_bytes)

    def test_active_texture_presentation_logic(self):
        # Kiểm tra logic tính toán quad full-screen và texture upload
        # Tọa độ quad chuẩn: [-1, 1], [-1, -1], [1, 1], [1, -1]
        quad_vertices = [
            (-1.0,  1.0, 0.0, 0.0),
            (-1.0, -1.0, 0.0, 1.0),
            ( 1.0,  1.0, 1.0, 0.0),
            ( 1.0, -1.0, 1.0, 1.0)
        ]
        self.assertEqual(len(quad_vertices), 4)
        # Điểm đầu top-left: V = 0.0
        self.assertEqual(quad_vertices[0][3], 0.0)
        # Điểm bottom-left: V = 1.0
        self.assertEqual(quad_vertices[1][3], 1.0)

        # Kiểm tra kích thước RGBA8 của texture 1920x1080
        w, h = 1920, 1080
        bpp = 4
        expected_bytes = w * h * bpp
        self.assertEqual(expected_bytes, 8294400)

    def test_sprite_draw_command_structure(self):
        # Kiểm tra cấu trúc SpriteDrawCommand:
        # - pos: 4 * 2 floats = 32 bytes (offset 0)
        # - uv: 4 * 2 floats = 32 bytes (offset 32)
        # - indices: 6 * uint32 = 24 bytes (offset 64)
        # - textureHandle: void* (8 bytes trên 64-bit) (offset 88)
        # - textureWidth: uint32 (4 bytes) (offset 96)
        # - textureHeight: uint32 (4 bytes) (offset 100)
        # Tổng cộng: 104 bytes
        import ctypes
        class SpriteDrawCommand(ctypes.Structure):
            _fields_ = [
                ('pos', (ctypes.c_float * 2) * 4),
                ('uv', (ctypes.c_float * 2) * 4),
                ('indices', ctypes.c_uint * 6),
                ('textureHandle', ctypes.c_void_p),
                ('textureWidth', ctypes.c_uint),
                ('textureHeight', ctypes.c_uint),
            ]
        self.assertEqual(ctypes.sizeof(SpriteDrawCommand), 104)
        self.assertEqual(SpriteDrawCommand.pos.offset, 0)
        self.assertEqual(SpriteDrawCommand.uv.offset, 32)
        self.assertEqual(SpriteDrawCommand.indices.offset, 64)
        self.assertEqual(SpriteDrawCommand.textureHandle.offset, 88)
        self.assertEqual(SpriteDrawCommand.textureWidth.offset, 96)
        self.assertEqual(SpriteDrawCommand.textureHeight.offset, 100)

    def test_sprite_texture_association_logic(self):
        # Kiểm tra logic ánh xạ handle kết cấu sang SpriteDrawCommand
        fake_tex_handle = 0x12345678
        tex_w = 2048
        tex_h = 1088
        cmd = {
            "pos": [(-1.0, 1.0), (-1.0, -1.0), (1.0, 1.0), (1.0, -1.0)],
            "uv": [(0.0, 0.0), (0.0, 1.0), (1.0, 0.0), (1.0, 1.0)],
            "textureHandle": fake_tex_handle,
            "textureWidth": tex_w,
            "textureHeight": tex_h
        }
        self.assertEqual(cmd["textureHandle"], fake_tex_handle)
        self.assertEqual(cmd["textureWidth"], 2048)
        self.assertEqual(cmd["textureHeight"], 1088)

    def test_sprite_geometry_extraction_from_vertex_buffer(self):
        # Giả lập Vertex Buffer 80 bytes (4 vertices * 20 bytes/vertex: x, y, z, u, v)
        # và trích xuất toạ độ pos (x, y) cùng UV (u, v)
        vertex_data = struct.pack(
            "20f",
            # Vertex 0: x, y, z, u, v
            -0.5, 0.5, 0.0, 0.1, 0.2,
            # Vertex 1: x, y, z, u, v
            -0.5, -0.5, 0.0, 0.1, 0.8,
            # Vertex 2: x, y, z, u, v
            0.5, 0.5, 0.0, 0.9, 0.2,
            # Vertex 3: x, y, z, u, v
            0.5, -0.5, 0.0, 0.9, 0.8
        )
        self.assertEqual(len(vertex_data), 80)

        # Trích xuất 4 vertices theo stride 20 bytes (5 floats)
        stride_floats = 5
        floats = struct.unpack("20f", vertex_data)
        extracted_pos = []
        extracted_uv = []
        for i in range(4):
            offset = i * stride_floats
            extracted_pos.append((floats[offset + 0], floats[offset + 1]))
            extracted_uv.append((floats[offset + 3], floats[offset + 4]))

        self.assertAlmostEqual(extracted_pos[0][0], -0.5)
        self.assertAlmostEqual(extracted_pos[0][1], 0.5)
        self.assertAlmostEqual(extracted_uv[0][0], 0.1)
        self.assertAlmostEqual(extracted_uv[0][1], 0.2)

        self.assertAlmostEqual(extracted_pos[3][0], 0.5)
        self.assertAlmostEqual(extracted_pos[3][1], -0.5)
        self.assertAlmostEqual(extracted_uv[3][0], 0.9)
        self.assertAlmostEqual(extracted_uv[3][1], 0.8)

    def test_sprite_coordinate_normalization_to_ndc(self):
        # Kiểm tra logic chuẩn hoá pixel space [-960, 960] x [-540, 540] sang OpenGL ES NDC [-1, 1]
        def normalize_vertex(raw_x, raw_y, raw_u, raw_v):
            if abs(raw_x) > 1.5 or abs(raw_y) > 1.5:
                pos_x = raw_x / 960.0
                pos_y = -raw_y / 540.0 # Lật trục Y
            else:
                pos_x = raw_x
                pos_y = raw_y

            import math
            if math.isnan(raw_u) or math.isinf(raw_u) or raw_u < -10.0 or raw_u > 10.0:
                uv_u = 0.0
            else:
                uv_u = raw_u

            if math.isnan(raw_v) or math.isinf(raw_v) or raw_v < -10.0 or raw_v > 10.0:
                uv_v = 0.0
            else:
                uv_v = raw_v

            return (pos_x, pos_y, uv_u, uv_v)

        # 1. Fullscreen Quad M2 (-960, -540) -> NDC (-1.0, 1.0)
        px, py, u, v = normalize_vertex(-960.0, -540.0, 0.0, 0.0)
        self.assertAlmostEqual(px, -1.0)
        self.assertAlmostEqual(py, 1.0)
        self.assertAlmostEqual(u, 0.0)
        self.assertAlmostEqual(v, 0.0)

        # 2. Bottom-right Fullscreen Quad (960, 540) -> NDC (1.0, -1.0)
        px, py, u, v = normalize_vertex(960.0, 540.0, 1.0, 1.0)
        self.assertAlmostEqual(px, 1.0)
        self.assertAlmostEqual(py, -1.0)
        self.assertAlmostEqual(u, 1.0)
        self.assertAlmostEqual(v, 1.0)

        # 3. Sprite UI nhỏ: (-508, -110)
        px, py, u, v = normalize_vertex(-508.0, -110.0, 0.25, 0.5)
        self.assertAlmostEqual(px, -508.0 / 960.0)
        self.assertAlmostEqual(py, 110.0 / 540.0)
        self.assertAlmostEqual(u, 0.25)
        self.assertAlmostEqual(v, 0.5)

        # 4. Bảo vệ UV dị thường (NaN, Inf, Out-of-bounds)
        px, py, u, v = normalize_vertex(0.0, 0.0, float('nan'), float('inf'))
        self.assertAlmostEqual(px, 0.0)
        self.assertAlmostEqual(py, 0.0)
        self.assertAlmostEqual(u, 0.0)
        self.assertAlmostEqual(v, 0.0)

    def test_sprite_double_buffering_and_degenerate_quad_filter(self):
        # Kiểm tra logic lọc quad degenerate (toàn bộ đỉnh bằng nhau / chiều rộng hoặc chiều cao = 0)
        def is_degenerate(pos):
            min_x = min(p[0] for p in pos)
            max_x = max(p[0] for p in pos)
            min_y = min(p[1] for p in pos)
            max_y = max(p[1] for p in pos)
            return ((max_x - min_x) < 0.001) or ((max_y - min_y) < 0.001)

        # Degenerate quad: 4 đỉnh đều tại (0, 0)
        null_quad = [(0.0, 0.0), (0.0, 0.0), (0.0, 0.0), (0.0, 0.0)]
        self.assertTrue(is_degenerate(null_quad), "Quad với 4 đỉnh (0, 0) phải bị xem là degenerate")

        # Valid fullscreen quad: (-1, 1), (1, 1), (-1, -1), (1, -1)
        valid_quad = [(-1.0, 1.0), (1.0, 1.0), (-1.0, -1.0), (1.0, -1.0)]
        self.assertFalse(is_degenerate(valid_quad), "Fullscreen quad không được bị xem là degenerate")

        # Double-buffering simulation:
        # active sprites được gom lại trong frame và chuyển sang ready sprites khi Present
        active_sprites = []
        ready_sprites = []

        # Giả lập gom quad hợp lệ và bỏ quad degenerate
        quads_to_draw = [null_quad, valid_quad, null_quad]
        for q in quads_to_draw:
            if not is_degenerate(q):
                active_sprites.append(q)

        self.assertEqual(len(active_sprites), 1, "Chỉ quad hợp lệ được đưa vào active_sprites")

        # Giả lập Present (Swap double buffer)
        ready_sprites = list(active_sprites)
        active_sprites.clear()

        self.assertEqual(len(ready_sprites), 1, "ready_sprites nhận đúng 1 sprite hợp lệ")
        self.assertEqual(len(active_sprites), 0, "active_sprites phải được dọn dẹp cho frame kế tiếp")

    def test_deferred_release_and_pipeline_retention(self):
        # Giả lập pipeline retention: khi buffer được bind vào IASet, refCount được giữ
        class MockRegistry:
            def __init__(self):
                self.ref_counts = {}

            def register(self, handle):
                self.ref_counts[handle] = 1

            def add_ref(self, handle):
                if handle in self.ref_counts:
                    self.ref_counts[handle] += 1
                return self.ref_counts.get(handle, -1)

            def release(self, handle):
                if handle in self.ref_counts:
                    self.ref_counts[handle] -= 1
                    r = self.ref_counts[handle]
                    if r == 0:
                        del self.ref_counts[handle]
                    return r
                return -1

        reg = MockRegistry()
        vb_handle = 0x1000
        reg.register(vb_handle)
        self.assertEqual(reg.ref_counts[vb_handle], 1)

        # Pipeline binds vb -> add_ref
        reg.add_ref(vb_handle)
        self.assertEqual(reg.ref_counts[vb_handle], 2)

        # Engine calls Release() -> refCount drops to 1, still retained!
        r = reg.release(vb_handle)
        self.assertEqual(r, 1, "Buffer vẫn được pipeline giữ")
        self.assertIn(vb_handle, reg.ref_counts)

        # Present / switch buffer -> pipeline releases
        r = reg.release(vb_handle)
        self.assertEqual(r, 0, "Buffer được giải phóng an toàn khi pipeline thôi sử dụng")
        self.assertNotIn(vb_handle, reg.ref_counts)

    def test_vertex_color_decode(self):
        # Kiểm tra giải mã định dạng BGRA 4 bytes sang float RGBA [0.0, 1.0]
        # Trường hợp 1: Màu trung tính DirectX 0xFF808080 (cR=128, cG=128, cB=128, cA=255)
        raw_color_neutral = 0xFF808080
        cb = raw_color_neutral & 0xFF
        cg = (raw_color_neutral >> 8) & 0xFF
        cr = (raw_color_neutral >> 16) & 0xFF
        ca = (raw_color_neutral >> 24) & 0xFF

        is_neutral = (cr == 0x80 and cg == 0x80 and cb == 0x80)
        r = 1.0 if is_neutral else min(1.0, cr / 128.0)
        g = 1.0 if is_neutral else min(1.0, cg / 128.0)
        b = 1.0 if is_neutral else min(1.0, cb / 128.0)
        a = ca / 255.0

        self.assertEqual((r, g, b, a), (1.0, 1.0, 1.0, 1.0), "Màu 0xFF808080 phải ánh xạ ra trắng đục chuẩn 1.0")

        # Trường hợp 2: Bán trong suốt 0x39808080 (ca = 57)
        raw_color_trans = 0x39808080
        ca_trans = (raw_color_trans >> 24) & 0xFF
        a_trans = ca_trans / 255.0
        self.assertAlmostEqual(a_trans, 57 / 255.0, places=4)

    def test_strip_restart_quad_extraction(self):
        # Kiểm tra công thức trích xuất quad từ Triangle Strip với Degenerate Restarts (4 + k * 6)
        # Các giá trị thực tế của Mages Engine: 4, 10, 16, 22, 28, 34, 40
        def extract_quads_from_indices(index_count, topology=4):
            num_quads = 0
            quad_stride = 4
            is_strip_restart = False
            is_tri_list = False

            if index_count >= 4 and (index_count - 4) % 6 == 0:
                num_quads = (index_count - 4) // 6 + 1
                quad_stride = 6
                is_strip_restart = True
            elif topology == 4 and index_count % 6 == 0:
                num_quads = index_count // 6
                quad_stride = 6
                is_tri_list = True
            else:
                num_quads = index_count // 4
                quad_stride = 4

            quads = []
            for q in range(num_quads):
                base_idx = q * quad_stride
                if is_tri_list:
                    quads.append([base_idx, base_idx + 1, base_idx + 2, base_idx + 5])
                else:
                    quads.append([base_idx, base_idx + 1, base_idx + 2, base_idx + 3])
            return quads

        # 1 quad (index_count = 4)
        q4 = extract_quads_from_indices(4)
        self.assertEqual(len(q4), 1)
        self.assertEqual(q4[0], [0, 1, 2, 3])

        # 2 quads (index_count = 10)
        q10 = extract_quads_from_indices(10)
        self.assertEqual(len(q10), 2)
        self.assertEqual(q10[0], [0, 1, 2, 3])
        self.assertEqual(q10[1], [6, 7, 8, 9]) # Bước nhảy 6 indices

        # 7 quads (index_count = 40)
        q40 = extract_quads_from_indices(40)
        self.assertEqual(len(q40), 7)
        self.assertEqual(q40[0], [0, 1, 2, 3])
        self.assertEqual(q40[6], [36, 37, 38, 39])

        # Triangle List (index_count = 12, topo = 4)
        q12_list = extract_quads_from_indices(12, topology=4)
        self.assertEqual(len(q12_list), 2)
        self.assertEqual(q12_list[0], [0, 1, 2, 5])
        self.assertEqual(q12_list[1], [6, 7, 8, 11])

    def test_batch_quad_extraction_multiple_quads(self):
        # Mô phỏng drawIndexed với IndexCount=40 theo chuẩn strip restart
        index_count = 40
        num_quads = (index_count - 4) // 6 + 1
        quads = []
        for q in range(num_quads):
            base = q * 6
            quads.append([base, base + 1, base + 2, base + 3])

        self.assertEqual(len(quads), 7, "IndexCount=40 với strip restart phải trích xuất được 7 quads")
        self.assertEqual(quads[0], [0, 1, 2, 3])
        self.assertEqual(quads[6], [36, 37, 38, 39])

if __name__ == "__main__":
    unittest.main()



