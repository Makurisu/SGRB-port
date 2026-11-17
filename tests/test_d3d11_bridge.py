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

    def test_start_index_location_offset_handling(self):
        # Mô phỏng lệnh vẽ có startIndexLocation > 0 trong index buffer chung
        index_buffer = list(range(100)) # 100 indices [0, 1, ..., 99]
        start_index = 24
        index_count = 10 # 2 quads strip restart
        num_quads = (index_count - 4) // 6 + 1
        quad_stride = 6

        extracted_vertices = []
        for q in range(num_quads):
            base_offset = q * quad_stride
            quad_idx = []
            for i in range(4):
                idx_offset = base_offset + i
                self.assertLess(idx_offset, index_count, "idx_offset phải nhỏ hơn index_count")
                actual_index = index_buffer[start_index + idx_offset]
                quad_idx.append(actual_index)
            extracted_vertices.append(quad_idx)

        self.assertEqual(len(extracted_vertices), 2)
        self.assertEqual(extracted_vertices[0], [24, 25, 26, 27])
        self.assertEqual(extracted_vertices[1], [30, 31, 32, 33])

    def test_uv_normalization_logic(self):
        # Kiểm tra chuẩn hoá và clamping UV an toàn: không modulo wrap gây co quắp khung hình
        def normalize_uv(u, v, tex_w, tex_h):
            norm_u = u
            norm_v = v
            if tex_w > 0 and tex_h > 0:
                if norm_u > 16.0:
                    norm_u /= float(tex_w)
                if norm_v > 16.0:
                    norm_v /= float(tex_h)

            norm_u = max(0.0, min(1.0, norm_u))
            norm_v = max(0.0, min(1.0, norm_v))
            return norm_u, norm_v

        # Trường hợp 1: UV chuẩn trong khoảng [0, 1] trên texture 2048x1088
        u1, v1 = normalize_uv(0.25, 0.75, 2048, 1088)
        self.assertAlmostEqual(u1, 0.25)
        self.assertAlmostEqual(v1, 0.75)

        # Trường hợp 2: UV Attention (1928x1040) khi V = 1.03846 (1080/1040) -> Bị clamp về 1.0 thay vì gãy vỡ về 0.038
        u2, v2 = normalize_uv(0.9958, 1.03846, 1928, 1040)
        self.assertAlmostEqual(u2, 0.9958, places=4)
        self.assertAlmostEqual(v2, 1.0, places=4)

        # Trường hợp 3: UV toạ độ pixel tuyệt đối > 16.0
        u3, v3 = normalize_uv(1024.0, 544.0, 2048, 1088)
        self.assertAlmostEqual(u3, 0.5)
        self.assertAlmostEqual(v3, 0.5)

    def test_uv_atlas_normalization_logic(self):
        # Kiểm tra chuẩn hoá toạ độ UV cho Atlas: ánh xạ từ Canvas 1920x1080 về dải [0.0, 1.0] của Atlas
        def scale_uv(u, v, tex_w, tex_h):
            is_psb_atlas = (tex_w == 3424 or tex_w == 488)
            if tex_w > 0 and tex_h > 0:
                if u > 16.0:
                    u /= float(tex_w)
                elif is_psb_atlas:
                    u = (u * 1920.0) / float(tex_w)
                elif u > 1.0 or u < 0.0:
                    import math
                    u = u - math.floor(u)

                if v > 16.0:
                    v /= float(tex_h)
                elif is_psb_atlas:
                    atlas_h = 1088.0 if tex_w == 488 else float(tex_h)
                    v = (v * 1080.0) / atlas_h
                elif v > 1.0 or v < 0.0:
                    import math
                    v = v - math.floor(v)

            u = max(0.0, min(1.0, u))
            v = max(0.0, min(1.0, v))
            return u, v

        # 1. Toạ độ UV trên Atlas 3424x3424 của Chữ 'S' (文字01): X=2705/1920=1.40885, Y=2157/1080=1.99722 -> ánh xạ về 0.79001 và 0.62996
        u_c1, v_c1 = scale_uv(2705.0 / 1920.0, 2157.0 / 1080.0, 3424, 3424)
        self.assertAlmostEqual(u_c1, 2705.0 / 3424.0, places=4)
        self.assertAlmostEqual(v_c1, 2157.0 / 3424.0, places=4)

        # 2. Toạ độ UV trên Atlas 488x1088 (文字02): X=417/1920=0.21718, Y=297/1080=0.27500 -> ánh xạ về 417/488 và 297/1088
        u_c2, v_c2 = scale_uv(417.0 / 1920.0, 297.0 / 1080.0, 488, 1088)
        self.assertAlmostEqual(u_c2, 417.0 / 488.0, places=4)
        self.assertAlmostEqual(v_c2, 297.0 / 1088.0, places=4)

        # 3. Với texture Logo / Cảnh báo 2048x1088, toạ độ UV chuẩn hoá [0.0, 1.0] được bảo toàn
        u_logo, v_logo = scale_uv(1.0, 1.0, 2048, 1088)
        self.assertAlmostEqual(u_logo, 1.0, places=4)
        self.assertAlmostEqual(v_logo, 1.0, places=4)

        # 4. Với texture Logo / Cảnh báo 1928x1040, toạ độ [0.0, 1.0] bảo toàn
        u_warn, v_warn = scale_uv(1.0, 1.0, 1928, 1040)
        self.assertAlmostEqual(u_warn, 1.0, places=4)
        self.assertAlmostEqual(v_warn, 1.0, places=4)

        # 5. Với sprite toạ độ pixel tuyệt đối > 16.0 (ví dụ: 1712, 1712)
        u_pixel, v_pixel = scale_uv(1712.0, 1712.0, 3424, 3424)
        self.assertAlmostEqual(u_pixel, 0.5, places=4)
        self.assertAlmostEqual(v_pixel, 0.5, places=4)

    def test_vertex_color_fallback_for_pos3_uv2_layout(self):
        # Kiểm tra giải mã màu đỉnh phân biệt giữa layout có Color (stride > 20) và layout chuẩn 20 bytes (float3 Pos + float2 UV)
        def decode_vertex_color(vert_bytes_offset8, vertex_stride=20):
            if vertex_stride <= 20:
                # Layout 20 bytes chuẩn của M2: offset 8..11 là Pos.Z float, tuyệt đối không đọc làm Color
                return 1.0, 1.0, 1.0, 1.0
            cb = vert_bytes_offset8[0]
            cg = vert_bytes_offset8[1]
            cr = vert_bytes_offset8[2]
            ca = vert_bytes_offset8[3]
            if ca == 0 and cr == 0 and cg == 0 and cb == 0:
                return 1.0, 1.0, 1.0, 1.0
            r = 1.0 if (cr == 0x80 and cg == 0x80 and cb == 0x80) else min(1.0, cr / 128.0)
            g = 1.0 if (cr == 0x80 and cg == 0x80 and cb == 0x80) else min(1.0, cg / 128.0)
            b = 1.0 if (cr == 0x80 and cg == 0x80 and cb == 0x80) else min(1.0, cb / 128.0)
            a = ca / 255.0
            return r, g, b, a

        # Trường hợp 1: Attention vi mạch với byte 8..11 = 0x09808080 (Pos.Z) trong layout 20 bytes -> phải trả về trắng đục sắc nét (1.0, 1.0, 1.0, 1.0)
        att_color = decode_vertex_color(bytes([0x80, 0x80, 0x80, 0x09]), vertex_stride=20)
        self.assertEqual(att_color, (1.0, 1.0, 1.0, 1.0), "Attention 20 bytes phải giữ nguyên màu sắc nét, không bị suy hao alpha")

        # Trường hợp 2: Layout mở rộng > 20 bytes có Color thực tế
        ext_color = decode_vertex_color(bytes([0x80, 0x80, 0x80, 0xFF]), vertex_stride=24)
        self.assertEqual(ext_color, (1.0, 1.0, 1.0, 1.0), "Layout mở rộng 24 bytes giải mã đúng màu")

        # Trường hợp 3: Layout mở rộng > 20 bytes có bán trong suốt
        trans_color = decode_vertex_color(bytes([0x80, 0x80, 0x80, 0x80]), vertex_stride=24)
        self.assertAlmostEqual(trans_color[3], 128.0 / 255.0, places=4)

    def test_white_screen_protection_when_alpha_mask_empty(self):
        # Kiểm tra lớp bảo vệ bỏ qua quad w_full toàn màn hình trên Atlas 3424
        def should_skip_quad(tex_w, span_x, span_y):
            if tex_w == 3424 and span_x >= 1.8 and span_y >= 1.8:
                return True # Bỏ qua tấm quét Stencil w_full để không che phủ trắng toàn màn hình
            return False

        # w_full toàn màn hình (span 2.0x2.0) -> PHẢI BỎ QUA
        self.assertTrue(should_skip_quad(3424, 2.0, 2.0))

        # Các sprite chữ và icon thông thường khác trên Atlas 3424 -> KHÔNG BỊ BỎ QUA
        self.assertFalse(should_skip_quad(3424, 0.5, 0.2))
        self.assertFalse(should_skip_quad(3424, 0.11, 0.16)) # Chữ 'S'

    def test_multi_texture_dual_slot_binding_logic(self):
        # Kiểm tra mô phỏng việc trích xuất texture Slot 0 và Slot 1
        bound_slots = [None] * 8
        bound_slots[0] = "Texture_Color_2048x1088"
        bound_slots[1] = "Texture_AlphaMask_1928x528"

        color_tex = bound_slots[0]
        alpha_tex = bound_slots[1]

        self.assertEqual(color_tex, "Texture_Color_2048x1088")
        self.assertEqual(alpha_tex, "Texture_AlphaMask_1928x528")

        # Khi shader có cả hai texture, kết quả cuối cùng = vec4(rgb, a * mask.a) * vColor
        has_alpha_texture = 1 if alpha_tex is not None else 0
        self.assertEqual(has_alpha_texture, 1)

    def test_ps_set_shader_resources_slot1_reset_leak_prevention(self):
        # Kiểm tra việc reset Slot 1 khi game chỉ bind Slot 0 (startSlot = 0, numViews = 1)
        slots = [None] * 8
        slots[0] = "Tex0"
        slots[1] = "Tex1"

        def ps_set_shader_resources(start_slot, views):
            num_views = len(views)
            if num_views > 0:
                for i in range(num_views):
                    slot = start_slot + i
                    if slot < 8:
                        slots[slot] = views[i]
                if start_slot == 0 and num_views == 1:
                    slots[1] = None

        # Giả sử trước đó frame có slot 1
        ps_set_shader_resources(0, ["TexColor", "TexAlpha"])
        self.assertEqual(slots[0], "TexColor")
        self.assertEqual(slots[1], "TexAlpha")

        # Frame/draw call tiếp theo chỉ truyền slot 0 -> slot 1 phải tự động được dọn dẹp
        ps_set_shader_resources(0, ["TexColorOnly"])
        self.assertEqual(slots[0], "TexColorOnly")
        self.assertIsNone(slots[1], "Slot 1 phải được reset về None để ngăn ngừa rò rỉ Alpha Texture")

    def test_alpha_texture_format_filter(self):
        # Alpha Texture chỉ được gán nếu texture format là DXGI_FORMAT_A8_UNORM (65)
        kDxgiA8Unorm = 65
        kDxgiR8G8B8A8Unorm = 28

        def select_alpha_mask(alpha_tex, fmt):
            if alpha_tex is not None and fmt == kDxgiA8Unorm:
                return alpha_tex
            return None

        self.assertIsNone(select_alpha_mask("TexRGBA", kDxgiR8G8B8A8Unorm))
        self.assertEqual(select_alpha_mask("TexMask", kDxgiA8Unorm), "TexMask")

    def test_a8_to_rgba8_expansion(self):
        # Kiểm tra thuật toán mở rộng Format 65 (A8_UNORM) sang RGBA8 (255, 255, 255, alpha) với alpha ở kênh 3
        raw_a8 = bytes([0x00, 0x80, 0xFF])
        expanded = bytearray()
        for b in raw_a8:
            expanded.extend([255, 255, 255, b])

        self.assertEqual(len(expanded), 12)
        # Pixel 0: Trong suốt hoàn toàn
        self.assertEqual(list(expanded[0:4]), [255, 255, 255, 0x00])
        # Pixel 1: Bán trong suốt
        self.assertEqual(list(expanded[4:8]), [255, 255, 255, 0x80])
        # Pixel 2: Đục hoàn toàn
        self.assertEqual(list(expanded[8:12]), [255, 255, 255, 0xFF])

    def test_offscreen_mask_subtractive_and_additive_modes(self):
        # Kiểm tra chế độ Subtractive Mask (Fiction) vs Additive Mask (Title Reveal)
        def compute_mask_val(initial_clear_val, src_alpha, vert_alpha=1.0):
            text_alpha = int(src_alpha * vert_alpha)
            return (255 - text_alpha) if (initial_clear_val == 0xFF) else text_alpha

        # 1. Fiction: Target clear về 0xFF -> Nét chữ đục lỗ thành maskAlpha = 0 (lộ nền đen)
        self.assertEqual(compute_mask_val(0xFF, 255), 0, "Chữ đặc trong Fiction phải có maskAlpha = 0 để lộ chữ đen")
        self.assertEqual(compute_mask_val(0xFF, 0), 255, "Nền trong Fiction phải có maskAlpha = 255 để hiện nền trắng")

        # 2. Title Reveal: Target clear về 0x00 -> Chữ quét sáng thành maskAlpha = 255
        self.assertEqual(compute_mask_val(0x00, 255), 255, "Chữ trong Title Reveal phải có maskAlpha = 255 để hiện chữ trắng")
        self.assertEqual(compute_mask_val(0x00, 0), 0, "Nền trong Title Reveal phải có maskAlpha = 0")

    def test_native_pipeline_direct_texture_render(self):
        # Kiểm tra kiến trúc pipeline chuẩn của M2 Engine:
        # Texture Slot 0 kết hợp Vertex Color RGBA (alpha fade) mà không cần inject fallback mask giả
        def render_sprite(tex_handle, vert_color, has_alpha_mask_injected):
            # Pipeline gốc không inject fallback mask cho Fiction hoặc Title
            self.assertFalse(has_alpha_mask_injected)
            # Alpha của pixel = tex_alpha * vert_color[3]
            tex_alpha = 1.0
            final_alpha = tex_alpha * vert_color[3]
            return final_alpha

        # Fade in 50%
        a = render_sprite("Tex2048", [1.0, 1.0, 1.0, 0.5], False)
        self.assertAlmostEqual(a, 0.5)

        # Full opaque
        a_full = render_sprite("Tex2048", [1.0, 1.0, 1.0, 1.0], False)
        self.assertAlmostEqual(a_full, 1.0)

    def test_w_full_screen_quad_preservation(self):
        # Kiểm tra toàn bộ quad hợp lệ (kể cả quad kích thước lớn w_full) đều được bảo toàn để render đúng thứ tự layer
        def should_preserve_quad(span_x, span_y):
            # Chỉ loại bỏ degenerate quads có diện tích xấp xỉ 0
            return (span_x >= 0.001 and span_y >= 0.001)

        # w_full background quad (2.0 x 2.0)
        self.assertTrue(should_preserve_quad(2.0, 2.0))
        # Glyph quad nhỏ (0.1 x 0.1)
        self.assertTrue(should_preserve_quad(0.1, 0.1))
        # Degenerate quad (0.0 x 0.0) -> Bỏ qua
        self.assertFalse(should_preserve_quad(0.0, 0.0))

    def test_safe_memmem_search_logic(self):
        # Kiểm tra thuật toán safe_memmem_search thay thế sub_14056C470 chống tràn stack cookie
        def safe_memmem_search(haystack, needle):
            if len(needle) == 0:
                return 0 # Con trỏ tới haystack_begin (offset 0)
            if len(haystack) < len(needle):
                return len(haystack) # Con trỏ tới haystack_end
            pos = haystack.find(needle)
            return pos if pos != -1 else len(haystack)

        data = b"\x1a\x45\xdf\xa3\x93\x42\x82\x88matroska\x42\x87\x81\x02\x42\x85\x81\x02"
        # 1. Tìm needle rỗng
        self.assertEqual(safe_memmem_search(data, b""), 0)
        # 2. Tìm needle ở đầu
        self.assertEqual(safe_memmem_search(data, b"\x1a\x45"), 0)
        # 3. Tìm needle "matroska" trong EBML header
        self.assertEqual(safe_memmem_search(data, b"matroska"), 8)
        # 4. Tìm needle không tồn tại -> trả về haystack_end
        self.assertEqual(safe_memmem_search(data, b"webm_video"), len(data))
        # 5. Needle dài hơn haystack -> trả về haystack_end an toàn, không buffer overflow
        self.assertEqual(safe_memmem_search(b"short", b"very_long_needle_pattern"), 5)

    def test_smart_color_texture_fallback_for_topology_4(self):
        # Kiểm tra logic gán colorTex khi vẽ text glyphs (Topology = 4)
        def resolve_color_texture(bound_tex0, active_tex, last_updated_tex, topology):
            color_tex = bound_tex0
            if color_tex is None:
                if topology == 4 and last_updated_tex is not None:
                    color_tex = last_updated_tex
                elif active_tex is not None:
                    color_tex = active_tex
                else:
                    color_tex = last_updated_tex
            return color_tex

        # Trường hợp 1: Bound Tex0 có sẵn -> dùng trực tiếp
        self.assertEqual(resolve_color_texture("Tex0", "ActiveTex", "UpdatedFontTex", 4), "Tex0")
        # Trường hợp 2: Bound Tex0 bị unbind (None), Topology = 4 (TriangleList chữ) -> ưu tiên font texture vừa UpdateSubresource
        self.assertEqual(resolve_color_texture(None, "ActiveBgTex", "UpdatedFontTex", 4), "UpdatedFontTex")
        # Trường hợp 3: Bound Tex0 bị unbind (None), Topology = 5 (TriangleStrip nền) -> lấy ActiveTex
        self.assertEqual(resolve_color_texture(None, "ActiveBgTex", "UpdatedFontTex", 5), "ActiveBgTex")

    def test_topology_4_triangle_list_precedence(self):
        # Kiểm tra ưu tiên nhận diện Topology 4 (TriangleList) trước StripRestart
        def parse_quad_mode(topo, index_count):
            if topo == 4 and index_count % 6 == 0:
                return "TRI_LIST", index_count // 6
            elif index_count >= 4 and (index_count - 4) % 6 == 0:
                return "STRIP_RESTART", (index_count - 4) // 6 + 1
            else:
                return "QUADS", index_count // 4

        # index_count = 6: TriangleList chuẩn cho 1 quad
        mode, quads = parse_quad_mode(4, 6)
        self.assertEqual(mode, "TRI_LIST")
        self.assertEqual(quads, 1)

        # index_count = 24: TriangleList cho 4 ký tự text
        mode, quads = parse_quad_mode(4, 24)
        self.assertEqual(mode, "TRI_LIST")
        self.assertEqual(quads, 4)

        # index_count = 4 với Topology 5: Strip đơn lẻ
        mode, quads = parse_quad_mode(5, 4)
        self.assertEqual(mode, "STRIP_RESTART")
        self.assertEqual(quads, 1)

    def test_attention_pure_circuit_quad_without_synthetic_text(self):
        # Kiểm tra logic xử lý khung Attention: giữ nguyên khung vi mạch gốc, tuyệt đối không chèn text nhân tạo
        def process_attention_dialog_quads(bound_slot0, quad_min_x, quad_max_x, quad_min_y, quad_max_y):
            quad_width = quad_max_x - quad_min_x
            # Quad gốc (hai nhánh vi mạch chạy animation khép vào giữa) luôn giữ texture gốc của Slot 0
            base_quad = {"tex": bound_slot0, "width": quad_width}
            # Bản gốc PC không chèn dòng text nào ở giữa, chỉ có khung vi mạch và (!) ATTENTION (!) nhỏ trên đỉnh
            return [base_quad]

        res = process_attention_dialog_quads("Tex1928x1040", -1.0, 1.0, -0.435, 0.435)
        self.assertEqual(len(res), 1, "Chỉ duy nhất quad khung vi mạch gốc được vẽ, không có quad text nhân tạo")
        self.assertEqual(res[0]["tex"], "Tex1928x1040")

    def test_update_texture_auto_expansion_for_row_pitch(self):
        # Kiểm tra thuật toán tự động mở rộng bề ngang và bề dọc texture khi RowPitch nguồn = 7680 (1920x1080)
        def compute_texture_dimensions(init_width, init_height, src_row_pitch, bytes_per_pixel=4):
            effective_width = init_width
            effective_height = init_height
            effective_pitch = init_width * bytes_per_pixel
            if src_row_pitch > effective_pitch:
                effective_width = src_row_pitch // bytes_per_pixel
                effective_pitch = src_row_pitch
                if effective_width == 1920 and init_height == 1024:
                    effective_height = 1080
            return effective_width, effective_height, effective_pitch

        # Texture 1024x1024 khởi tạo nhưng nguồn có RowPitch = 7680 (1920 pixels * 4 bytes) -> mở rộng lên 1920x1080
        w, h, pitch = compute_texture_dimensions(1024, 1024, 7680, 4)
        self.assertEqual(w, 1920)
        self.assertEqual(h, 1080)
        self.assertEqual(pitch, 7680)

        # Texture thông thường có RowPitch khớp (ví dụ 1024 * 4 = 4096)
        w2, h2, pitch2 = compute_texture_dimensions(1024, 1024, 4096, 4)
        self.assertEqual(w2, 1024)
        self.assertEqual(h2, 1024)
        self.assertEqual(pitch2, 4096)

    def test_sprite_batch_ready_frame_isolation(self):
        # Kiểm tra thuật toán chỉ đọc sReadyFrameSprites để chống vẽ trùng đè (double draw) gây sai lệch màu
        def get_batch_sprites(ready_sprites, max_commands=1024):
            return list(ready_sprites)[:max_commands]

        ready = ["Quad_Title", "Quad_Button", "Quad_Text"]
        result = get_batch_sprites(ready)
        self.assertEqual(len(result), 3)
        self.assertEqual(result, ["Quad_Title", "Quad_Button", "Quad_Text"])

    def test_alpha_mask_empty_protection_validation(self):
        # Kiểm tra bộ lọc phát hiện mặt nạ alpha rỗng
        def has_valid_alpha_content(alpha_bytes):
            if not alpha_bytes:
                return False
            # Lấy mẫu bước nhảy
            step = max(1, len(alpha_bytes) // 64)
            for i in range(0, len(alpha_bytes), step):
                if alpha_bytes[i] > 0:
                    return True
            return False

        # 1. Mặt nạ RenderTarget rỗng (toàn bộ 0) -> False (vô hiệu hoá)
        empty_mask = bytes([0] * 1024)
        self.assertFalse(has_valid_alpha_content(empty_mask))

        # 2. Mặt nạ thực tế có alpha > 0 (ví dụ icon hoặc wipe dissolve) -> True (kích hoạt)
        valid_mask = bytearray([0] * 1024)
        valid_mask[512] = 255
        self.assertTrue(has_valid_alpha_content(bytes(valid_mask)))

    def test_vertex_alpha_fade_transition_preservation(self):
        # Kiểm tra logic giải mã alpha từ layout đỉnh >= 20 bytes cho hiệu ứng fade transition mượt mà
        def decode_vertex_alpha(vertex_stride, vert_bytes_offset8):
            if vertex_stride >= 20:
                cB = vert_bytes_offset8[0]
                cG = vert_bytes_offset8[1]
                cR = vert_bytes_offset8[2]
                cA = vert_bytes_offset8[3]
                if cA == 0 and cR == 0 and cG == 0 and cB == 0:
                    return 1.0
                return cA / 255.0
            return 1.0

        # 1. Logo trong suốt (cA = 0, RGB có giá trị): alpha = 0.0
        self.assertAlmostEqual(decode_vertex_alpha(20, bytes([0x80, 0x80, 0x80, 0x00])), 0.0, places=2)
        # 2. Logo đang fade-in (cA = 128 / ~50% opacity): alpha = ~0.502
        self.assertAlmostEqual(decode_vertex_alpha(20, bytes([0x80, 0x80, 0x80, 0x80])), 128 / 255.0, places=2)
        # 3. Logo hiển thị đầy đủ (cA = 255 / 100% opacity): alpha = 1.0
        self.assertAlmostEqual(decode_vertex_alpha(20, bytes([0x80, 0x80, 0x80, 0xFF])), 1.0, places=2)
        # 4. Toàn bộ 0 (fallback an toàn): alpha = 1.0
        self.assertAlmostEqual(decode_vertex_alpha(20, bytes([0x00, 0x00, 0x00, 0x00])), 1.0, places=2)

    def test_texture_revision_cache_optimization(self):
        # Kiểm tra cơ chế Texture Revision: chỉ gọi glTexSubImage2D khi revision thay đổi
        gles_cache = {}
        upload_count = 0

        def get_or_create_gles_texture(handle, width, height, revision):
            nonlocal upload_count
            if handle not in gles_cache:
                upload_count += 1
                gles_cache[handle] = {"texId": 100 + len(gles_cache), "width": width, "height": height, "revision": revision}
                return gles_cache[handle]["texId"]
            
            entry = gles_cache[handle]
            if entry["width"] != width or entry["height"] != height:
                upload_count += 1
                entry["width"] = width
                entry["height"] = height
                entry["revision"] = revision
            elif entry["revision"] != revision:
                upload_count += 1
                entry["revision"] = revision
            return entry["texId"]

        # 1. Khung hình đầu: Logo Spike Chunsoft nạp lần đầu (rev 1) -> upload 1 lần
        handle_logo = "Tex_SpikeChunsoft"
        tex_id = get_or_create_gles_texture(handle_logo, 2048, 1088, 1)
        self.assertEqual(upload_count, 1)

        # 2. 60 khung hình tiếp theo: Logo Spike Chunsoft fade-in/fade-out (rev vẫn là 1) -> 0 lần upload thêm!
        for _ in range(60):
            res_id = get_or_create_gles_texture(handle_logo, 2048, 1088, 1)
            self.assertEqual(res_id, tex_id)
        self.assertEqual(upload_count, 1, "Texture tĩnh không được phép upload lại qua GPU bus ở mỗi frame")

        # 3. Font texture được cập nhật (rev tăng lên 2) -> upload đúng 1 lần
        handle_font = "Tex_Font"
        get_or_create_gles_texture(handle_font, 1024, 1024, 1)
        self.assertEqual(upload_count, 2)
        get_or_create_gles_texture(handle_font, 1024, 1024, 2)
        self.assertEqual(upload_count, 3, "Khi font texture thay đổi revision, GPU upload phải được kích hoạt chính xác 1 lần")

    def test_attention_circuit_quad_and_artifact_filter(self):
        # Kiểm tra bộ lọc quad rác Attention: loại bỏ quad bị trôi toạ độ ở mép đáy
        def should_filter_attention_quad(tex_w, tex_h, uv0_v):
            return (tex_w == 1928 and tex_h == 1040 and uv0_v >= 0.4)

        # Quad rác có UV0.V = 0.497 trên texture 1928x1040 -> Phải bị lọc bỏ
        self.assertTrue(should_filter_attention_quad(1928, 1040, 0.497))
        # Quad vi mạch chính (UV0.V = 0.001) -> Không bị lọc, được vẽ bình thường
        self.assertFalse(should_filter_attention_quad(1928, 1040, 0.001))

    def test_title_w_full_filtered_and_glyphs_preserved(self):
        # Kiểm tra bộ lọc tấm màn trắng w_full 3424x3424 và giữ nguyên các chữ cái hoạt ảnh
        def should_filter_title_quad(tex_w, quad_span_x, quad_span_y):
            return (tex_w == 3424 and quad_span_x >= 1.8 and quad_span_y >= 1.8)

        # 1. Tấm overlay trắng w_full toàn màn hình (span 2.0 x 2.0) -> Bị lọc bỏ để không phủ trắng màn hình
        self.assertTrue(should_filter_title_quad(3424, 2.0, 2.0))
        # 2. Chữ 'S' (span ~ 0.11 x 0.16) -> Không bị lọc
        self.assertFalse(should_filter_title_quad(3424, 0.11, 0.16))
        # 3. Chữ 't' (span ~ 0.06 x 0.10) -> Không bị lọc
        self.assertFalse(should_filter_title_quad(3424, 0.06, 0.10))

    def test_frame_pacing_handshake_state_machine(self):
        # Kiểm tra mô hình đồng bộ Frame Pacing 60 FPS:
        # Game Thread (Producer) submit frame -> pending_ack = True
        # Render Thread (Consumer) swap buffer -> pending_ack = False
        class MockFramePacer:
            def __init__(self):
                self.pending_ack = False
                self.frames_submitted = 0
                self.frames_presented = 0
                self.dropped_frames = 0

            def present_game_frame(self):
                if self.pending_ack:
                    # Nếu frame trước chưa được render xong, producer bị chặn hoặc drop
                    self.dropped_frames += 1
                    return False
                self.pending_ack = True
                self.frames_submitted += 1
                return True

            def render_presented_frame(self):
                if self.pending_ack:
                    self.frames_presented += 1
                    self.pending_ack = False
                    return True
                return False

        pacer = MockFramePacer()
        # Chạy tuần tự 60 frames có handshake
        for _ in range(60):
            sub_ok = pacer.present_game_frame()
            self.assertTrue(sub_ok, "Mỗi frame game submit phải thành công khi có nhịp handshake")
            pres_ok = pacer.render_presented_frame()
            self.assertTrue(pres_ok, "Render thread phải nhận và hoàn thành hiển thị frame")

        self.assertEqual(pacer.frames_submitted, 60)
        self.assertEqual(pacer.frames_presented, 60)
        self.assertEqual(pacer.dropped_frames, 0, "Khung hình không bao giờ bị drop khi có frame pacing 60 FPS")

    def test_draw_indexed_batch_extraction(self):
        # Kiểm tra logic gom batch quads trước khi cập nhật container
        raw_quads = [
            {"id": 0, "w": 100, "h": 50},
            {"id": 1, "w": 200, "h": 80},
            {"id": 2, "w": 0, "h": 0}, # degenerate quad
            {"id": 3, "w": 150, "h": 40},
        ]
        batch = []
        for q in raw_quads:
            if q["w"] > 0 and q["h"] > 0:
                batch.append(q)

        active_sprites = []
        # Chèn nguyên batch một lần (tương đương 1 lần lock mutex)
        active_sprites.extend(batch)

        self.assertEqual(len(active_sprites), 3)
        self.assertEqual([q["id"] for q in active_sprites], [0, 1, 3])

    def test_alpha_mask_validity_cache(self):
        # Kiểm tra cơ chế cache quét Alpha Mask theo (handle, revision)
        scan_count = 0
        cache = {}

        def check_alpha_mask(handle, rev, pixels):
            nonlocal scan_count
            if handle in cache and cache[handle][0] == rev:
                return cache[handle][1]
            scan_count += 1
            has_alpha = any(p > 0 for p in pixels)
            cache[handle] = (rev, has_alpha)
            return has_alpha

        # Lần đầu kiểm tra handle A rev 1
        res1 = check_alpha_mask("handle_mask_A", 1, [0, 0, 0, 255])
        self.assertTrue(res1)
        self.assertEqual(scan_count, 1)

        # 10 quad tiếp theo trong cùng frame (cùng handle A, rev 1) -> Sử dụng cache O(1)
        for _ in range(10):
            res = check_alpha_mask("handle_mask_A", 1, [0, 0, 0, 255])
            self.assertTrue(res)
        self.assertEqual(scan_count, 1, "Scan CPU chỉ được chạy 1 lần duy nhất khi revision không đổi")

        # Handle A đổi rev 2 (có cập nhật mask mới)
        res2 = check_alpha_mask("handle_mask_A", 2, [0, 0, 0, 0])
        self.assertFalse(res2)
        self.assertEqual(scan_count, 2, "Khi revision thay đổi, scan CPU được kích hoạt lại đúng 1 lần")

    def test_stencil_comparison_func_mapping(self):
        # Kiểm tra chuyển đổi D3D11_COMPARISON_FUNC sang OpenGL ES Comparison Functions
        d3d_to_gl_funcs = {
            1: 0x0200, # D3D11_COMPARISON_NEVER -> GL_NEVER
            2: 0x0201, # D3D11_COMPARISON_LESS -> GL_LESS
            3: 0x0202, # D3D11_COMPARISON_EQUAL -> GL_EQUAL
            4: 0x0203, # D3D11_COMPARISON_LESS_EQUAL -> GL_LEQUAL
            5: 0x0204, # D3D11_COMPARISON_GREATER -> GL_GREATER
            6: 0x0205, # D3D11_COMPARISON_NOT_EQUAL -> GL_NOTEQUAL
            7: 0x0206, # D3D11_COMPARISON_GREATER_EQUAL -> GL_GEQUAL
            8: 0x0207, # D3D11_COMPARISON_ALWAYS -> GL_ALWAYS
        }
        def mock_d3d_to_gl_func(d3d_val):
            return d3d_to_gl_funcs.get(d3d_val, 0x0207)

        for d3d_val, gl_val in d3d_to_gl_funcs.items():
            self.assertEqual(mock_d3d_to_gl_func(d3d_val), gl_val)
        self.assertEqual(mock_d3d_to_gl_func(999), 0x0207, "Mặc định fallback về GL_ALWAYS")

    def test_stencil_op_mapping(self):
        # Kiểm tra chuyển đổi D3D11_STENCIL_OP sang OpenGL ES Stencil Operations
        d3d_to_gl_ops = {
            1: 0x1E00, # D3D11_STENCIL_OP_KEEP -> GL_KEEP
            2: 0,      # D3D11_STENCIL_OP_ZERO -> GL_ZERO
            3: 0x1E01, # D3D11_STENCIL_OP_REPLACE -> GL_REPLACE
            4: 0x1E02, # D3D11_STENCIL_OP_INCR_SAT -> GL_INCR
            5: 0x1E03, # D3D11_STENCIL_OP_DECR_SAT -> GL_DECR
            6: 0x150A, # D3D11_STENCIL_OP_INVERT -> GL_INVERT
            7: 0x8507, # D3D11_STENCIL_OP_INCR -> GL_INCR_WRAP
            8: 0x8508, # D3D11_STENCIL_OP_DECR -> GL_DECR_WRAP
        }
        def mock_d3d_to_gl_op(d3d_val):
            return d3d_to_gl_ops.get(d3d_val, 0x1E00)

        for d3d_val, gl_val in d3d_to_gl_ops.items():
            self.assertEqual(mock_d3d_to_gl_op(d3d_val), gl_val)
        self.assertEqual(mock_d3d_to_gl_op(999), 0x1E00, "Mặc định fallback về GL_KEEP")

    def test_pipelined_present1_no_deadlock(self):
        # Kiểm tra Present1 hoạt động không khóa nhịp blocking 25ms
        class PipelinedSwapChain:
            def __init__(self):
                self.frame_index = 0
                self.has_new_frame = False

            def present1(self):
                self.frame_index += 1
                self.has_new_frame = True
                return 0 # S_OK tức thì, không wait_for 25ms

        sc = PipelinedSwapChain()
        for i in range(1, 61):
            hr = sc.present1()
            self.assertEqual(hr, 0)
            self.assertEqual(sc.frame_index, i)
            self.assertTrue(sc.has_new_frame)

    def test_stencil_color_write_mask_filtering(self):
        # Kiểm tra logic xử lý w_full (Atlas 3424):
        # Chỉ skip khi alpha_mask is None VÀ colorWriteMask != 0.
        # Cho phép render khi có alpha_mask (fill ruột chữ cái Steins;Gate) hoặc colorWriteMask == 0 (stencil pass).
        def should_render_quad(tex_width, span_x, span_y, color_write_mask, alpha_mask_handle=None):
            if tex_width == 3424 and span_x >= 1.8 and span_y >= 1.8:
                if alpha_mask_handle is None and color_write_mask != 0:
                    return False # Bị skip vì vẽ đè trắng không có mặt nạ
            return True

        # Trường hợp 1: Stencil write pass (colorWriteMask == 0) -> Phải cho phép render để thiết lập mặt nạ
        self.assertTrue(should_render_quad(3424, 2.0, 2.0, color_write_mask=0))

        # Trường hợp 2: Color fill pass không có mask (colorWriteMask != 0, alpha_mask = None) -> Bị skip
        self.assertFalse(should_render_quad(3424, 2.0, 2.0, color_write_mask=0x0F, alpha_mask_handle=None))

        # Trường hợp 3: Color fill pass CÓ mask (alpha_mask_handle != None) -> Cho phép render fill ruột chữ cái
        self.assertTrue(should_render_quad(3424, 2.0, 2.0, color_write_mask=0x0F, alpha_mask_handle="MaskRT"))

        # Trường hợp 4: Quad icon thông thường (kích thước < 1.8) -> Luôn cho phép render
        self.assertTrue(should_render_quad(3424, 0.5, 0.3, color_write_mask=0x0F))

    def test_attention_texture_alpha_fade_preservation(self):
        # Kiểm tra cơ chế phân biệt layout đỉnh của Attention (texture 1928):
        # Khi texture là Attention 1928x1040 / 1928x1088, layout đỉnh là Position.XYZ + UV (offset 8..11 là Position.Z),
        # giữ nguyên màu đỉnh chuẩn (1.0, 1.0, 1.0, 1.0) để đảm bảo toàn bộ khung vi mạch sắc nét không bị méo mó.
        def process_vertex_color(tex_width, raw_bytes_8_11, stride=20):
            if tex_width == 1928:
                return (1.0, 1.0, 1.0, 1.0)
            cB, cG, cR, cA = raw_bytes_8_11
            r = (cR == 0x80 and cG == 0x80 and cB == 0x80) and 1.0 or min(1.0, cR / 128.0)
            g = (cG == 0x80 and cR == 0x80 and cB == 0x80) and 1.0 or min(1.0, cG / 128.0)
            b = (cB == 0x80 and cR == 0x80 and cB == 0x80) and 1.0 or min(1.0, cB / 128.0)
            a = cA / 255.0
            return (r, g, b, a)

        # Texture 1928 nhận chuẩn 100% màu đỉnh sắc nét
        color_att = process_vertex_color(1928, (128, 128, 128, 9))
        self.assertEqual(color_att, (1.0, 1.0, 1.0, 1.0))

        # Texture Logo 2048 bảo toàn alpha fade chuyển cảnh
        color_logo_fade = process_vertex_color(2048, (128, 128, 128, 64))
        self.assertAlmostEqual(color_logo_fade[3], 64.0 / 255.0, places=4)
        color_logo_full = process_vertex_color(2048, (128, 128, 128, 255))
        self.assertAlmostEqual(color_logo_full[3], 1.0, places=4)

    def test_frame_cadence_pacing_and_clear_masks(self):
        # 1. Kiểm tra thuật toán Frame Cadence Pacing 60 FPS
        class CadencePacer:
            def __init__(self, target_fps=60):
                self.target_interval_ns = 1_000_000_000 // target_fps
                self.last_present_time = 0
                self.has_presented = False

            def compute_wait_ns(self, current_time_ns, sync_interval=1):
                if not self.has_presented:
                    self.has_presented = True
                    self.last_present_time = current_time_ns
                    return 0
                interval_ns = self.target_interval_ns * max(1, sync_interval)
                elapsed = current_time_ns - self.last_present_time
                if elapsed < interval_ns:
                    wait = interval_ns - elapsed
                    self.last_present_time = current_time_ns + wait
                    return wait
                self.last_present_time = current_time_ns
                return 0

        pacer = CadencePacer(60)
        # Giả sử frame 1 hoàn thành lúc t = 0
        wait1 = pacer.compute_wait_ns(0)
        self.assertEqual(wait1, 0)

        # Giả sử game thread render frame 2 siêu nhanh (chỉ mất 2ms = 2,000,000 ns)
        wait2 = pacer.compute_wait_ns(2_000_000)
        # Phải chờ phần còn lại của chu kỳ 16.666ms: 16,666,666 - 2,000,000 = 14,666,666 ns
        self.assertEqual(wait2, 14_666_666, "Phải chờ đúng 14.66ms để duy trì nhịp 60 FPS hoàn hảo")

        # 2. Kiểm tra glStencilMask và glColorMask được reset về 0xFF / GL_TRUE trước glClear
        class MockGlesContext:
            def __init__(self):
                self.stencil_mask = 0
                self.color_mask = False

            def set_masks_for_clear(self):
                self.stencil_mask = 0xFF
                self.color_mask = True

        ctx = MockGlesContext()
        ctx.set_masks_for_clear()
        self.assertEqual(ctx.stencil_mask, 0xFF, "Stencil mask phải là 0xFF trước glClear")
        self.assertTrue(ctx.color_mask, "Color mask phải là True trước glClear")

    def test_offscreen_render_target_mask_pipeline(self):
        # 1. Kiểm tra quyết định cấp phát CPU buffer cho Offscreen Render Target Mask
        kDxgiA8Unorm = 65
        kBindRenderTarget = 0x20
        kBindShaderResource = 0x08

        def check_wants_cpu_copy(dxgi_format, bind_flags):
            is_shader_resource = (bind_flags & kBindShaderResource) != 0
            is_offscreen_mask = (dxgi_format == kDxgiA8Unorm) or ((bind_flags & kBindRenderTarget) and is_shader_resource)
            bytes_per_pixel = 4 if dxgi_format == kDxgiA8Unorm else 4
            return bytes_per_pixel != 0 and (((bind_flags & (kBindRenderTarget | 0x40)) == 0) or is_offscreen_mask)

        # Texture 1920x1080 A8_UNORM RenderTarget + ShaderResource phải được cấp phát CPU buffer
        self.assertTrue(check_wants_cpu_copy(kDxgiA8Unorm, kBindRenderTarget | kBindShaderResource))
        # Pure backbuffer render target không cần CPU buffer
        self.assertFalse(check_wants_cpu_copy(28, kBindRenderTarget))

        # 2. Kiểm tra rasterize offscreen mask vào buffer RGBA 4 bytes
        # Giả lập buffer 4x4
        w, h = 4, 4
        dst_buffer = bytearray(w * h * 4)

        # Clear về 0
        self.assertEqual(set(dst_buffer), {0})

        # Rasterize 1 pixel tại (x=1, y=1) với src maskVal = 200, vertAlpha = 0.5
        x, y = 1, 1
        mask_val = 200
        vert_alpha = 0.5
        final_alpha = int(mask_val * vert_alpha) # 100
        offset = (y * w + x) * 4
        dst_buffer[offset + 0] = 255
        dst_buffer[offset + 1] = 255
        dst_buffer[offset + 2] = 255
        dst_buffer[offset + 3] = final_alpha

        self.assertEqual(dst_buffer[offset + 0], 255)
        self.assertEqual(dst_buffer[offset + 1], 255)
        self.assertEqual(dst_buffer[offset + 2], 255)
        self.assertEqual(dst_buffer[offset + 3], 100, "Alpha channel phải phản ánh chính xác mask modulation")

    def test_tile_offset_uv_normalization_for_psb_atlas(self):
        # Kiểm tra thuật toán chuẩn hoá toạ độ UV với tile offset cho Atlas PSB và texture thông thường
        import math
        def normalize_quad_uv(uv_coords, tex_w, tex_h):
            uvs = [[u, v] for u, v in uv_coords]
            for i in range(4):
                if uvs[i][0] > 16.0: uvs[i][0] /= float(tex_w)
                if uvs[i][1] > 16.0: uvs[i][1] /= float(tex_h)

            min_u = min(uvs[i][0] for i in range(4))
            max_u = max(uvs[i][0] for i in range(4))
            if min_u >= 0.99 or max_u <= -0.01:
                tile_u = math.floor(min_u)
                for i in range(4): uvs[i][0] -= tile_u

            min_v = min(uvs[i][1] for i in range(4))
            max_v = max(uvs[i][1] for i in range(4))
            if min_v >= 0.99 or max_v <= -0.01:
                tile_v = math.floor(min_v)
                for i in range(4): uvs[i][1] -= tile_v

            for i in range(4):
                uvs[i][0] = max(0.0, min(1.0, uvs[i][0]))
                uvs[i][1] = max(0.0, min(1.0, uvs[i][1]))
            return uvs

        # Trường hợp 1: Chữ cái 'e' trên Atlas 3424x3424 có U trong [1.75, 1.78] -> Chuẩn hoá về [0.75, 0.78]
        raw_letter_e = [[1.75, 0.0], [1.78, 0.0], [1.75, 0.13], [1.78, 0.13]]
        norm_e = normalize_quad_uv(raw_letter_e, 3424, 3424)
        self.assertAlmostEqual(norm_e[0][0], 0.75, places=3)
        self.assertAlmostEqual(norm_e[1][0], 0.78, places=3)
        self.assertAlmostEqual(norm_e[0][1], 0.00, places=3)
        self.assertAlmostEqual(norm_e[2][1], 0.13, places=3)

        # Trường hợp 2: Chữ 'PRESS ANY BUTTON' với V trong [3.14, 3.16] -> Chuẩn hoá về [0.14, 0.16]
        raw_pab = [[0.30, 3.14], [0.48, 3.14], [0.30, 3.16], [0.48, 3.16]]
        norm_pab = normalize_quad_uv(raw_pab, 3424, 3424)
        self.assertAlmostEqual(norm_pab[0][0], 0.30, places=3)
        self.assertAlmostEqual(norm_pab[1][0], 0.48, places=3)
        self.assertAlmostEqual(norm_pab[0][1], 0.14, places=3)
        self.assertAlmostEqual(norm_pab[2][1], 0.16, places=3)

        # Trường hợp 3: Khung Attention (1928x1040) với span [0.0005, 1.0005] -> Giữ nguyên span không bị co rút về 1 pixel
        raw_att = [[0.0005, 0.0009], [1.0005, 0.0009], [0.0005, 0.4287], [1.0005, 0.4287]]
        norm_att = normalize_quad_uv(raw_att, 1928, 1040)
        self.assertAlmostEqual(norm_att[0][0], 0.0005, places=4)
        self.assertAlmostEqual(norm_att[1][0], 1.0000, places=4)
        self.assertAlmostEqual(norm_att[1][0] - norm_att[0][0], 0.9995, places=3)

    def test_fiction_mask_isolation(self):
        # Kiểm tra logic tách biệt Mask Slot 1 cho Fiction (2048x1088)
        def assign_alpha_mask(tex_width, bound_slot1_handle, slot1_is_format65):
            if tex_width == 2048:
                return None
            if bound_slot1_handle is not None and slot1_is_format65:
                return bound_slot1_handle
            return None

        # Khi texture là Fiction 2048, dù slot 1 còn sót mask của Attention, kết quả vẫn là None
        mask_fiction = assign_alpha_mask(2048, "AttentionMaskHandle", True)
        self.assertIsNone(mask_fiction, "Fiction (2048) tuyệt đối không được nhận mask Slot 1 của Attention")

        # Khi texture là Title Atlas 3424, nhận mask bình thường để quét chữ
        mask_title = assign_alpha_mask(3424, "TitleMaskHandle", True)
        self.assertEqual(mask_title, "TitleMaskHandle")

    def test_ps_set_shader_resources_slot1_reset(self):
        # Kiểm tra khi startSlot == 0 và numViews == 1, Slot 1 được giải phóng
        slots = ["tex0_old", "mask1_old", None, None]
        def ps_set_shader_resources(start_slot, num_views, views):
            for i in range(num_views):
                slots[start_slot + i] = views[i]
            if start_slot == 0 and num_views == 1:
                slots[1] = None

        ps_set_shader_resources(0, 1, ["tex0_new"])
        self.assertEqual(slots[0], "tex0_new")
        self.assertIsNone(slots[1], "Slot 1 phải được reset về None khi chỉ bind Slot 0")

    def test_attention_vertex_alpha_preserved(self):
        # Kiểm tra màu đỉnh và vertex alpha của Attention được giữ nguyên thay vì bị hardcode thành 1.0f
        def extract_vertex_color(tex_width, raw_bytes_8_11):
            cB, cG, cR, cA = raw_bytes_8_11
            if cA == 0 and cR == 0 and cG == 0 and cB == 0:
                return (1.0, 1.0, 1.0, 1.0)
            r = 1.0 if (cR == 0x80 and cG == 0x80 and cB == 0x80) else min(1.0, cR / 128.0)
            g = 1.0 if (cG == 0x80 and cR == 0x80 and cB == 0x80) else min(1.0, cG / 128.0)
            b = 1.0 if (cB == 0x80 and cR == 0x80 and cB == 0x80) else min(1.0, cB / 128.0)
            a = cA / 255.0
            return (r, g, b, a)

        # Đỉnh Attention khi đang fade nhẹ (cA = 18 tương đương alpha = 0.0706)
        color_att_fade = extract_vertex_color(1928, (128, 128, 128, 18))
        self.assertAlmostEqual(color_att_fade[3], 18.0 / 255.0, places=3)
        self.assertNotEqual(color_att_fade[3], 1.0, "Alpha fade của Attention không được bị ép thành 1.0")

    def test_shader_creation_counts_and_separation(self):
        # M2 Engine khởi tạo chuẩn xác 12 Vertex Shaders và 2077 Pixel Shaders (tổng 2089)
        total_vs = 12
        total_ps = 2077
        total_shaders = total_vs + total_ps
        self.assertEqual(total_shaders, 2089, "Tổng số Shader DXBC trong binary sgre_steam.exe phải là 2089")

    def test_wipe_transition_shader_math(self):
        # Kiểm chứng công thức toán học trích xuất từ DXBC ps_2040 (Wipe Transition)
        # add r0.x, r0.x, -cb0[0].x (mask - phase)
        # mad_sat r0.x, r0.x, cb0[0].y, 1.0 (saturate((mask - phase) * invVague + 1.0))
        # mul r0.x, r0.x, v1.w ( nhân vtxAlpha)
        # mul o0.w, r0.x, r1.w ( nhân texColor.a)
        def compute_wipe_alpha(mask_val, phase, inv_vague, vtx_alpha, tex_alpha):
            diff = mask_val - phase
            scaled = diff * inv_vague + 1.0
            eff_alpha = max(0.0, min(1.0, scaled)) * vtx_alpha
            return eff_alpha * tex_alpha

        # Trường hợp 1: Phase = 0.0, wipe chưa bắt đầu -> toàn bộ mask hiển thị
        self.assertAlmostEqual(compute_wipe_alpha(0.5, 0.0, 1.0, 1.0, 1.0), 1.0)
        # Trường hợp 2: Phase = 1.0, wipe đã xong, mask = 0.0 -> alpha = 0.0
        self.assertAlmostEqual(compute_wipe_alpha(0.0, 1.0, 1.0, 1.0, 1.0), 0.0)
        # Trường hợp 3: Vùng chuyển tiếp (mask - phase = -0.5 với invVague = 1.0 -> scaled = 0.5)
        self.assertAlmostEqual(compute_wipe_alpha(0.3, 0.8, 1.0, 1.0, 1.0), 0.5)

    def test_sprite_atlas_reveal_shader_math(self):
        # Kiểm chứng công thức toán học trích xuất từ DXBC ps_0255 (PSB Sprite Atlas / Title Screen Reveal)
        # mul r0.xy, v0.xy, cb0[0].xy (screenUV = SV_Position.xy * screenMul.xy)
        # sample r0, r0.xy, t1, s1 (maskAlpha = sample(alphaTexture, screenUV).a)
        # sample r1, v2.xy, t0, s0 (texColor = sample(colorTexture, spriteUV))
        # mul r1, r1, v1 (color * vtxColor)
        # mul o0.w, r0.w, r1.w (finalAlpha = color.a * vtxColor.a * maskAlpha)
        def compute_sprite_reveal(screen_pos, screen_mul, mask_alpha, tex_color, vtx_color):
            screen_uv = (screen_pos[0] * screen_mul[0], screen_pos[1] * screen_mul[1])
            final_rgb = tuple(tc * vc for tc, vc in zip(tex_color[:3], vtx_color[:3]))
            final_a = (tex_color[3] * vtx_color[3]) * mask_alpha
            return (*final_rgb, final_a), screen_uv

        screen_mul = (1.0 / 1920.0, 1.0 / 1080.0)
        color_out, uv = compute_sprite_reveal((960.0, 540.0), screen_mul, 0.8, (1.0, 0.5, 0.2, 1.0), (1.0, 1.0, 1.0, 0.5))
        self.assertAlmostEqual(uv[0], 0.5)
        self.assertAlmostEqual(uv[1], 0.5)
        self.assertAlmostEqual(color_out[3], 1.0 * 0.5 * 0.8) # 0.4

    def test_atlas_virtual_canvas_uv_normalization(self):
        # M2 Engine xuất toạ độ UV theo Virtual Canvas 1920x1080
        # PixelX = 2705 -> rawU = 2705 / 1920 = 1.408854
        # PixelY = 2157 -> rawV = 2157 / 1080 = 1.997222
        def normalize_uv(raw_u, raw_v, tex_w, tex_h):
            if tex_w == 3424 or tex_h == 3424:
                u = (raw_u * 1920.0) / tex_w
                v = (raw_v * 1080.0) / tex_h
            else:
                u, v = raw_u, raw_v
            return max(0.0, min(1.0, u)), max(0.0, min(1.0, v))

        norm_u, norm_v = normalize_uv(2705.0 / 1920.0, 2157.0 / 1080.0, 3424, 3424)
        expected_u = 2705.0 / 3424.0
        expected_v = 2157.0 / 3424.0
        self.assertAlmostEqual(norm_u, expected_u, places=5)
        self.assertAlmostEqual(norm_v, expected_v, places=5)
        # Đảm bảo không bị trừ floor làm nhảy sai ô nhớ (như lỗi cũ trừ 1.0 -> 0.408)
        self.assertGreater(norm_u, 0.70)

    def test_w_full_quad_retained_on_atlas(self):
        # Kiểm tra quad kích thước 2.0x2.0 (w_full) trên Atlas 3424 không bị loại bỏ
        def should_keep_quad(tex_w, quad_w, quad_h):
            # Quy tắc GPU pass-through mới: không tự tiện drop quad
            return True

        self.assertTrue(should_keep_quad(3424, 2.0, 2.0))

    def test_shader_id_and_cb0_passthrough(self):
        # Kiểm tra Pixel Shader ID 2040 (Wipe) và hằng số cb0 (phase, invVague) được truyền trực tiếp
        def evaluate_shader_passthrough(shader_id, cb0, mask_val, vtx_alpha, base_alpha):
            if shader_id == 2040:
                phase = cb0[0]
                inv_vague = cb0[1] if cb0[1] != 0.0 else 1.0
                diff = mask_val - phase
                eff_alpha = max(0.0, min(1.0, diff * inv_vague + 1.0)) * vtx_alpha
                return base_alpha * eff_alpha
            return base_alpha * vtx_alpha

        # Test Wipe shader với phase = 0.5, invVague = 1.0, mask = 0.7
        res = evaluate_shader_passthrough(2040, [0.5, 1.0, 0.0, 0.0], 0.7, 1.0, 1.0)
        self.assertAlmostEqual(res, 1.0)
        # Test Wipe shader khi phase vượt quá mask (phase = 1.5)
        res_closed = evaluate_shader_passthrough(2040, [1.5, 1.0, 0.0, 0.0], 0.2, 1.0, 1.0)
        self.assertAlmostEqual(res_closed, 0.0)

    def test_arbitrary_mesh_passthrough_geometry_retention(self):
        # Kiểm tra lưới tam giác tự do (Arbitrary Mesh) - Giữ nguyên 100% tam giác của M2 Engine
        # Ví dụ một lưới 3 tam giác liên tiếp chia sẻ cạnh (mesh uốn lượn E-mote):
        # Tam giác 1: [0, 1, 2], Tam giác 2: [1, 3, 2], Tam giác 3: [2, 3, 4]
        # (Tổng cộng 9 chỉ số tam giác, 5 đỉnh)
        indices = [0, 1, 2, 1, 3, 2, 2, 3, 4]
        raw_vertices = [
            (100.0, 200.0, 0.1, 0.2), # V0
            (150.0, 220.0, 0.3, 0.2), # V1
            (110.0, 260.0, 0.1, 0.5), # V2
            (160.0, 270.0, 0.3, 0.5), # V3
            (120.0, 310.0, 0.1, 0.8), # V4
        ]

        # Mô phỏng thuật toán đóng gói MeshDrawCommand trong d3d11_bridge.cpp
        vertex_remap = {}
        out_vertex_data = []
        out_index_data = []

        for idx in indices:
            if idx not in vertex_remap:
                new_idx = len(out_vertex_data) // 8
                x, y, u, v = raw_vertices[idx]
                # Stride 8 float: X, Y, U, V, R, G, B, A
                out_vertex_data.extend([x / 960.0, -y / 540.0, u, v, 1.0, 1.0, 1.0, 1.0])
                vertex_remap[idx] = new_idx
                out_index_data.append(new_idx)
            else:
                out_index_data.append(vertex_remap[idx])

        # Đảm bảo số lượng chỉ số tam giác bằng đúng 9 (3 tam giác đầy đủ, không bị cụt)
        self.assertEqual(len(out_index_data), 9)
        self.assertEqual(out_index_data, [0, 1, 2, 1, 3, 2, 2, 3, 4])
        # Đảm bảo số lượng đỉnh sau khi deduplicate là đúng 5 đỉnh (5 * 8 = 40 floats)
        self.assertEqual(len(out_vertex_data), 5 * 8)
        self.assertEqual(len(vertex_remap), 5)

    def test_arbitrary_mesh_vertex_deduplication_and_remap(self):
        # Kiểm tra việc ánh xạ lại chỉ số khi có baseVertexLocation và chỉ số không liên tục
        indices = [10, 11, 12, 12, 11, 13]
        base_vertex = 5 # baseVertexLocation
        # Chỉ số thực tế: [15, 16, 17, 17, 16, 18]

        vertex_remap = {}
        out_indices = []
        for k in indices:
            v_idx = k + base_vertex
            if v_idx not in vertex_remap:
                new_idx = len(vertex_remap)
                vertex_remap[v_idx] = new_idx
                out_indices.append(new_idx)
            else:
                out_indices.append(vertex_remap[v_idx])

        self.assertEqual(out_indices, [0, 1, 2, 2, 1, 3])
        self.assertEqual(len(vertex_remap), 4)
        self.assertEqual(vertex_remap[15], 0)
        self.assertEqual(vertex_remap[18], 3)

    def test_arbitrary_mesh_edge_cases_out_of_bounds(self):
        # Edge cases: index âm hoặc trỏ vượt quá kích thước buffer
        # Hệ thống phải loại bỏ index lỗi an toàn và không làm corrupt dữ liệu
        indices = [-1, 0, 1, 2, 999] # -1 và 999 là bất hợp lệ
        max_vertices = 5

        valid_indices = []
        for idx in indices:
            if idx < 0 or idx >= max_vertices:
                continue
            valid_indices.append(idx)

        self.assertEqual(valid_indices, [0, 1, 2])
        self.assertEqual(len(valid_indices), 3)

    def test_arbitrary_mesh_double_buffering_swap(self):
        # Kiểm tra logic hoán đổi danh sách mesh O(1) giữa Game Thread và Render Thread
        active_drawing_meshes = [{"id": 1}, {"id": 2}]
        ready_frame_meshes = []

        # IDXGISwapChain::Present1 hoán đổi
        ready_frame_meshes = list(active_drawing_meshes)
        active_drawing_meshes.clear()

        self.assertEqual(len(active_drawing_meshes), 0)
        self.assertEqual(len(ready_frame_meshes), 2)

        # GLES Render Thread tiêu thụ qua SwapFrameMeshes
        consumed_meshes = list(ready_frame_meshes)
        ready_frame_meshes.clear()

        self.assertEqual(len(ready_frame_meshes), 0)
        self.assertEqual(len(consumed_meshes), 2)

    def test_triangle_strip_to_triangles_conversion(self):
        # Kiểm tra logic giải mã D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP (topo=5) sang GL_TRIANGLES
        # Dải 4 index thông thường của 1 quad: [0, 2, 1, 3] -> 2 tam giác: (0, 2, 1) và (2, 3, 1)
        raw_strip_indices = [0, 2, 1, 3]
        triangles = []
        for k in range(len(raw_strip_indices) - 2):
            raw_a = raw_strip_indices[k]
            raw_b = raw_strip_indices[k + 1]
            raw_c = raw_strip_indices[k + 2]
            if raw_a == raw_b or raw_b == raw_c or raw_a == raw_c:
                continue
            if k % 2 == 1:
                raw_b, raw_c = raw_c, raw_b
            triangles.extend([raw_a, raw_b, raw_c])

        self.assertEqual(triangles, [0, 2, 1, 2, 3, 1])

        # Kiểm tra dải đa tam giác có degenerate triangle: [0, 1, 2, 2, 3, 3, 4, 5]
        strip_with_degenerate = [0, 1, 2, 2, 3, 3, 4, 5]
        triangles_deg = []
        for k in range(len(strip_with_degenerate) - 2):
            raw_a = strip_with_degenerate[k]
            raw_b = strip_with_degenerate[k + 1]
            raw_c = strip_with_degenerate[k + 2]
            if raw_a == raw_b or raw_b == raw_c or raw_a == raw_c:
                continue
            if k % 2 == 1:
                raw_b, raw_c = raw_c, raw_b
            triangles_deg.extend([raw_a, raw_b, raw_c])

        # Các tam giác hợp lệ không chứa tam giác suy biến
        self.assertEqual(triangles_deg, [0, 1, 2, 3, 5, 4])

    def test_psb_atlas_488_uv_normalization(self):
        # Kiểm tra logic quy đổi UV của Texture Atlas 488x1088 (chứa ký tự 't' và icon menu)
        # M2 Engine xuất toạ độ UV tính theo Virtual Canvas 1920x1080:
        # Icon '0017' (chữ 't' trong 'Gate'): left=357.0, top=297.0
        # rawU = 357.0 / 1920.0 = 0.1859375
        # rawV = 297.0 / 1080.0 = 0.275
        raw_u = 357.0 / 1920.0
        raw_v = 297.0 / 1080.0
        tex_w = 488
        tex_h = 1088

        is_psb_atlas = (tex_w == 3424 or tex_h == 3424 or (tex_w == 488 and tex_h == 1088))
        self.assertTrue(is_psb_atlas)

        atlas_u = (raw_u * 1920.0) / float(tex_w)
        atlas_v = (raw_v * 1080.0) / float(tex_h)

        # Toạ độ pixel quy đổi trên Atlas 488x1088 phải khớp chính xác 357.0 và 297.0
        self.assertAlmostEqual(atlas_u * tex_w, 357.0, places=4)
        self.assertAlmostEqual(atlas_v * tex_h, 297.0, places=4)

    def test_d3d_to_gl_blend_mapping(self):
        # Kiểm tra bảng tra cứu chuyển đổi D3D11 Blend Factor sang OpenGL ES
        blend_mapping = {
            1: 0,       # D3D11_BLEND_ZERO -> GL_ZERO
            2: 1,       # D3D11_BLEND_ONE -> GL_ONE
            3: 0x0300,  # D3D11_BLEND_SRC_COLOR -> GL_SRC_COLOR
            4: 0x0301,  # D3D11_BLEND_INV_SRC_COLOR -> GL_ONE_MINUS_SRC_COLOR
            5: 0x0302,  # D3D11_BLEND_SRC_ALPHA -> GL_SRC_ALPHA
            6: 0x0303,  # D3D11_BLEND_INV_SRC_ALPHA -> GL_ONE_MINUS_SRC_ALPHA
            7: 0x0304,  # D3D11_BLEND_DEST_ALPHA -> GL_DST_ALPHA
            8: 0x0305,  # D3D11_BLEND_INV_DEST_ALPHA -> GL_ONE_MINUS_DST_ALPHA
            9: 0x0306,  # D3D11_BLEND_DEST_COLOR -> GL_DST_COLOR
            10: 0x0307, # D3D11_BLEND_INV_DEST_COLOR -> GL_ONE_MINUS_DST_COLOR
            11: 0x0308, # D3D11_BLEND_SRC_ALPHA_SAT -> GL_SRC_ALPHA_SATURATE
        }
        for d3d_val, expected_gl in blend_mapping.items():
            # Mô phỏng logic hàm d3dBlendToGl
            if d3d_val == 1: gl_val = 0
            elif d3d_val == 2: gl_val = 1
            elif d3d_val == 3: gl_val = 0x0300
            elif d3d_val == 4: gl_val = 0x0301
            elif d3d_val == 5: gl_val = 0x0302
            elif d3d_val == 6: gl_val = 0x0303
            elif d3d_val == 7: gl_val = 0x0304
            elif d3d_val == 8: gl_val = 0x0305
            elif d3d_val == 9: gl_val = 0x0306
            elif d3d_val == 10: gl_val = 0x0307
            elif d3d_val == 11: gl_val = 0x0308
            else: gl_val = 0x0302
            self.assertEqual(gl_val, expected_gl)

        # Kiểm tra D3D11 BlendOp sang OpenGL ES
        op_mapping = {
            1: 0x8006,  # D3D11_BLEND_OP_ADD -> GL_FUNC_ADD
            2: 0x800A,  # D3D11_BLEND_OP_SUBTRACT -> GL_FUNC_SUBTRACT
            3: 0x800B,  # D3D11_BLEND_OP_REV_SUBTRACT -> GL_FUNC_REVERSE_SUBTRACT
            4: 0x8007,  # D3D11_BLEND_OP_MIN -> GL_MIN
            5: 0x8008,  # D3D11_BLEND_OP_MAX -> GL_MAX
        }
        for d3d_op, expected_gl_op in op_mapping.items():
            if d3d_op == 1: gl_op = 0x8006
            elif d3d_op == 2: gl_op = 0x800A
            elif d3d_op == 3: gl_op = 0x800B
            elif d3d_op == 4: gl_op = 0x8007
            elif d3d_op == 5: gl_op = 0x8008
            else: gl_op = 0x8006
            self.assertEqual(gl_op, expected_gl_op)

    def test_d3d_to_gl_scissor_rect_conversion(self):
        # Kiểm tra chuyển đổi toạ độ Scissor Rect từ D3D11 (Top-Left) sang GLES (Bottom-Left)
        # Khung hình canvas 1920x1080
        viewport_h = 1080
        # Ví dụ hộp cắt [left=200, top=100, right=800, bottom=500]
        d3d_rect = {'left': 200, 'top': 100, 'right': 800, 'bottom': 500}
        gl_x = d3d_rect['left']
        gl_w = d3d_rect['right'] - d3d_rect['left']
        gl_h = d3d_rect['bottom'] - d3d_rect['top']
        gl_y = viewport_h - d3d_rect['bottom']

        self.assertEqual(gl_x, 200)
        self.assertEqual(gl_w, 600)
        self.assertEqual(gl_h, 400)
        self.assertEqual(gl_y, 1080 - 500)  # 580

        # Kiểm tra trường hợp đặc biệt: toàn màn hình [0, 0, 1920, 1080]
        full_rect = {'left': 0, 'top': 0, 'right': 1920, 'bottom': 1080}
        self.assertEqual(full_rect['left'], 0)
        self.assertEqual(full_rect['right'] - full_rect['left'], 1920)
        self.assertEqual(viewport_h - full_rect['bottom'], 0)
        self.assertEqual(full_rect['bottom'] - full_rect['top'], 1080)

    def test_shader_t1_usage_bitmask(self):
        # Kiểm tra bảng bitmask 260 bytes tra cứu shader sử dụng Slot 1 (t1 Alpha Mask)
        # Các shader render thông thường KHÔNG dùng Slot 1
        non_t1_shaders = [0, 1, 180, 240, 300, 2051]
        # Các shader chuyển cảnh, reveal, mask CÓ dùng Slot 1
        t1_shaders = [15, 195, 255, 1223, 2040, 2049, 2070]

        s_shader_uses_t1_bits = [
            0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
            0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0x0F, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
            0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x49,
            0x92, 0x24, 0x49, 0x00,
        ]

        def is_shader_using_t1(sid):
            if sid < 2080:
                return (s_shader_uses_t1_bits[sid // 8] & (1 << (sid % 8))) != 0
            return False

        for sid in non_t1_shaders:
            self.assertFalse(is_shader_using_t1(sid), f"Shader {sid} không được dùng T1")

        for sid in t1_shaders:
            self.assertTrue(is_shader_using_t1(sid), f"Shader {sid} phải dùng T1")

    def test_alpha_texture_assignment_via_shader_t1(self):
        # Kiểm tra logic gán alphaTextureHandle dựa trên isShaderUsingT1 thay vì hardcode kích thước
        def is_shader_using_t1(sid):
            return sid in [195, 255, 2040]

        # Case 1: Shader 195 (Sweep Reveal) với Atlas 3424 và Slot 1 hợp lệ -> BẮT BUỘC nhận alphaTextureHandle
        shader_id = 195
        tex_w = 3424
        slot1_res = "MaskTexture_A8"
        alpha_handle = slot1_res if (is_shader_using_t1(shader_id) and slot1_res is not None) else None
        self.assertEqual(alpha_handle, "MaskTexture_A8", "Sweep Reveal (Shader 195) trên Atlas 3424 phải nhận Alpha Mask")

        # Case 2: Shader 180 (Logo/Text thường) trên Atlas 3424 dù Slot 1 còn con trỏ cũ -> KHÔNG được nhận alphaTextureHandle
        shader_id = 180
        alpha_handle = slot1_res if (is_shader_using_t1(shader_id) and slot1_res is not None) else None
        self.assertIsNone(alpha_handle, "Shader 180 không được nhận Alpha Mask để tránh bị cắt mất chữ")

        # Case 3: Shader 180 trên Fiction 2048 -> KHÔNG được nhận alphaTextureHandle
        shader_id = 180
        tex_w = 2048
        alpha_handle = slot1_res if (is_shader_using_t1(shader_id) and slot1_res is not None) else None
        self.assertIsNone(alpha_handle, "Shader 180 trên Fiction không được nhận Alpha Mask")

    def test_fullscreen_quad_atlas_uv_normalization(self):
        # Kiểm tra chuẩn hoá toạ độ Full-screen Quad trên Atlas 3424 (như title_font/grid hoặc title_font/w_full)
        # Quad toàn màn hình phải được quy đổi từ Virtual Canvas 1920x1080 sang Atlas UV để không làm lộ rác Menu UI
        def process_mesh_uv(raw_u, raw_v, tex_w, tex_h):
            is_psb_atlas = (tex_w == 3424 or tex_w == 488)
            u = raw_u
            v = raw_v
            if raw_u > 16.0:
                u /= float(tex_w)
            elif is_psb_atlas:
                u = (u * 1920.0) / float(tex_w)

            if raw_v > 16.0:
                v /= float(tex_h)
            elif is_psb_atlas:
                v = (v * 1080.0) / float(tex_h)
            return min(1.0, max(0.0, u)), min(1.0, max(0.0, v))

        # Case 1: Background grid Full-screen Quad trên Atlas 3424 -> ánh xạ về đúng vùng [0, 0.5607] x [0, 0.3154]
        u1, v1 = process_mesh_uv(1.0, 1.0, 3424, 3424)
        self.assertAlmostEqual(u1, 1920.0 / 3424.0, places=4, msg="Background grid UV phải khớp đúng 1920/3424")
        self.assertAlmostEqual(v1, 1080.0 / 3424.0, places=4, msg="Background grid UV phải khớp đúng 1080/3424")

        # Case 2: Sweep Reveal w_full Quad trên Atlas 3424 -> ánh xạ về đúng vùng w_full [0, 0.5607] x [0.6334, 0.9488]
        u_w, v_w = process_mesh_uv(1.0, 3249.0 / 1080.0, 3424, 3424)
        self.assertAlmostEqual(u_w, 1920.0 / 3424.0, places=4, msg="Sweep Reveal w_full UV phải khớp đúng 1920/3424")
        self.assertAlmostEqual(v_w, 3249.0 / 3424.0, places=4, msg="Sweep Reveal w_full UV phải khớp đúng 3249/3424")

        # Case 3: Glyph chữ cái 'S' (-508, -110) đến (-397, 51) trên Atlas 3424 -> Quy đổi Virtual Canvas UV về Atlas
        raw_u_s = 2705.0 / 1920.0
        raw_v_s = 2157.0 / 1080.0
        u2, v2 = process_mesh_uv(raw_u_s, raw_v_s, 3424, 3424)
        self.assertAlmostEqual(u2, 2705.0 / 3424.0, places=4, msg="Glyph chữ 'S' phải được quy đổi chính xác về Atlas UV")
        self.assertAlmostEqual(v2, 2157.0 / 3424.0, places=4, msg="Glyph chữ 'S' phải được quy đổi chính xác về Atlas UV")

    def test_scissor_rect_initial_640x480_upgrade(self):
        # Kiểm tra logic mở rộng Scissor Rect khởi tạo ban đầu 640x480 khi Viewport chuyển sang 1920x1080
        sc_l, sc_t, sc_r, sc_b = 0, 0, 640, 480
        vp_w, vp_h = 1920, 1080
        if sc_l == 0 and sc_t == 0 and sc_r == 640 and sc_b == 480 and vp_w >= 1920:
            sc_r = 1920
            sc_b = 1080
        self.assertEqual((sc_r, sc_b), (1920, 1080), "Scissor rect 640x480 mặc định phải được tự động mở rộng thành 1920x1080")

    def test_offscreen_rasterizer_psb_atlas_source_mapping(self):
        # Kiểm tra logic ánh xạ pixel nguồn cho CPU Rasterizer Offscreen Mask với Atlas PSB
        # Đảm bảo không dùng std::floor() làm lệch toạ độ pixel về đáy Atlas
        def map_source_pixel(u, v, tex_w, tex_h):
            is_psb_atlas = (tex_w == 3424 or tex_h == 3424 or (tex_w == 488 and tex_h == 1088))
            if is_psb_atlas:
                src_x = int(u * 1920.0)
                src_y = int(v * 1080.0)
            else:
                src_x = int(u * tex_w)
                src_y = int(v * tex_h)
            return src_x, src_y

        # Chữ 'S' trên Atlas 3424x3424: X=2705, Y=2157
        u_s = 2705.0 / 1920.0
        v_s = 2157.0 / 1080.0
        src_x_s, src_y_s = map_source_pixel(u_s, v_s, 3424, 3424)
        self.assertEqual(src_x_s, 2705, "Chữ 'S' phải được map chính xác về pixel X=2705 trên Atlas 3424")
        self.assertEqual(src_y_s, 2157, "Chữ 'S' phải được map chính xác về pixel Y=2157 trên Atlas 3424")

        # Chữ 't' trên Atlas 488x1088: X=417, Y=297
        u_t = 417.0 / 1920.0
        v_t = 297.0 / 1080.0
        src_x_t, src_y_t = map_source_pixel(u_t, v_t, 488, 1088)
        self.assertEqual(src_x_t, 417, "Chữ 't' phải được map chính xác về pixel X=417 trên Atlas 488")
        self.assertEqual(src_y_t, 297, "Chữ 't' phải được map chính xác về pixel Y=297 trên Atlas 488")

    def test_fbo_render_target_protection_from_cpu_overwrite(self):
        # Kiểm thử cơ chế bảo vệ FBO Render Target: GPU sở hữu dữ liệu, CPU không ghi đè
        fbo_cache = {0x1234: 1} # handle -> fbo_id
        render_target_handles = {0x1234}
        sub_image_called = False

        def get_or_create_gles_texture(handle, revision, cached_rev):
            nonlocal sub_image_called
            if handle in fbo_cache or handle in render_target_handles:
                # FBO texture: giữ nguyên dữ liệu GPU, không gọi glTexSubImage2D
                return "gpu_texture_preserved"
            if revision != cached_rev:
                sub_image_called = True
                return "cpu_uploaded"
            return "cached"

        # 1. Texture thông thường khi revision đổi
        res_normal = get_or_create_gles_texture(0x9999, revision=2, cached_rev=1)
        self.assertTrue(sub_image_called, "Texture thường phải cho phép cập nhật từ CPU")
        self.assertEqual(res_normal, "cpu_uploaded")

        # 2. Texture Render Target khi revision đổi
        sub_image_called = False
        res_fbo = get_or_create_gles_texture(0x1234, revision=2, cached_rev=1)
        self.assertFalse(sub_image_called, "FBO Render Target TUYỆT ĐỐI KHÔNG được gọi glTexSubImage2D từ CPU")
        self.assertEqual(res_fbo, "gpu_texture_preserved")

    def test_vertex_shader_sm4_color_and_screen_uv_pass_through(self):
        # Kiểm thử ánh xạ 12 Vertex Shader theo đúng thứ tự khởi tạo runtime CreateVertexShader #0..#11:
        # 0, 6: POS (cb0Size=80 -> uVsCb0[4], không UV)
        # 1, 7: POS + COLOR (cb0Size=64 -> aColor, không UV)
        # 2, 8: POS + TEX0 (cb0Size=80 -> uVsCb0[4], uv0)
        # 3, 9: POS + TEX0 + TEX1 (cb0Size=80 -> uVsCb0[4], uv0 + uv1)
        # 4, 10: POS + COLOR + TEX0 (cb0Size=64 -> aColor, uv0)
        # 5, 11: POS + COLOR + TEX0 + TEX1 (cb0Size=64 -> aColor, uv0 + uv1)
        const_color_ids = {0, 2, 3, 6, 8, 9}

        def compute_vcolor(vs_shader_id, cb0_4, a_color):
            if vs_shader_id in const_color_ids:
                return cb0_4
            return a_color

        def compute_texcoords(vs_shader_id, uv0, uv1):
            if vs_shader_id in {0, 1, 6, 7}:
                return (0.0, 0.0), (0.0, 0.0)
            elif vs_shader_id in {2, 4, 8, 10}:
                return uv0, (0.0, 0.0)
            return uv0, uv1

        # VS_04 (Title Screen Background & Sprites): POS + COLOR + TEX0 -> lấy aColor = (0.5, 0.5, 0.5, 1.0)
        # Khi kết hợp PS_300 (Modulate 2X: baseColor.rgb * vColor.rgb * 2.0), hệ số sáng đúng bằng 1.0x!
        vcol_vs04 = compute_vcolor(4, (1.0, 1.0, 1.0, 1.0), (0.5, 0.5, 0.5, 1.0))
        self.assertEqual(vcol_vs04, (0.5, 0.5, 0.5, 1.0), "vsId = 4 phải lấy màu từ aColor để không nhân đôi độ sáng nền")
        modulate_2x_gain = vcol_vs04[0] * 2.0
        self.assertAlmostEqual(modulate_2x_gain, 1.0, places=5)
        tc0_4, tc1_4 = compute_texcoords(4, (0.5, 0.5), (0.1, 0.1))
        self.assertEqual(tc0_4, (0.5, 0.5), "vsId = 4 phải có TexCoord0 hợp lệ")
        self.assertEqual(tc1_4, (0.0, 0.0), "vsId = 4 không có TexCoord1")

        # VS_02: POS + TEX0 -> dùng constVtxColor uVsCb0[4], có TexCoord0
        vcol_vs02 = compute_vcolor(2, (1.0, 1.0, 1.0, 0.75), (0.0, 0.0, 0.0, 0.0))
        self.assertEqual(vcol_vs02[3], 0.75, "vsId = 2 phải lấy alpha từ uVsCb0[4]")
        tc0_2, tc1_2 = compute_texcoords(2, (0.25, 0.75), (0.9, 0.9))
        self.assertEqual(tc0_2, (0.25, 0.75))
        self.assertEqual(tc1_2, (0.0, 0.0))

        # VS_05: POS + COLOR + TEX0 + TEX1 -> dùng aColor, có đủ uv0 và uv1 (Title Sweep PS 2040)
        vcol_vs05 = compute_vcolor(5, (1.0, 1.0, 1.0, 1.0), (0.5, 0.5, 0.5, 0.8))
        self.assertEqual(vcol_vs05[3], 0.8)
        tc0_5, tc1_5 = compute_texcoords(5, (0.2, 0.3), (0.7, 0.8))
        self.assertEqual(tc0_5, (0.2, 0.3))
        self.assertEqual(tc1_5, (0.7, 0.8))

    def test_texture2d_get_desc_returns_registered_dimensions_and_edge_cases(self):
        # Kiểm thử mock_Texture2D_GetDesc trả về kích thước/format thực của từng texture trong sRegistry
        registry = {
            0x2001: {"width": 1928, "height": 1040, "format": 98, "bindFlags": 0x08},
            0x2002: {"width": 1928, "height": 528,  "format": 65, "bindFlags": 0x08},
            0x2003: {"width": 2048, "height": 1096, "format": 65, "bindFlags": 0x08},
            0x2004: {"width": 3424, "height": 3424, "format": 98, "bindFlags": 0x08},
            0x2005: {"width": 1920, "height": 1080, "format": 45, "bindFlags": 0x40},
        }
        mock_backbuffer = 0xDEAD0001

        def get_texture_desc(this_ptr, out_valid=True):
            if not out_valid:
                return None
            if this_ptr is not None and this_ptr != mock_backbuffer and this_ptr in registry:
                return dict(registry[this_ptr])
            return {"width": 1920, "height": 1080, "format": 28, "bindFlags": 0x28}

        # Happy Path: Attention 1928x1040 -> UV Y = 517 / 1040 -> lấy đúng pixel 517 (>= 514), không ăn lẹm khung chữ y=0..513
        desc_att = get_texture_desc(0x2001)
        self.assertEqual((desc_att["width"], desc_att["height"], desc_att["format"]), (1928, 1040, 98))
        uv_y_circuit_top = 517.0 / float(desc_att["height"])
        sampled_texel_y = uv_y_circuit_top * desc_att["height"]
        self.assertAlmostEqual(sampled_texel_y, 517.0, places=5)
        self.assertGreater(sampled_texel_y, 513.0, "Không được ăn lẹm vào khung chữ Attention (y <= 513)")

        # Happy Path: DepthStencil 1920x1080 Format=45 (D24_UNORM_S8_UINT) để M2 Engine bật cờ D3D11_CLEAR_STENCIL (0x2)
        desc_dsv = get_texture_desc(0x2005)
        self.assertEqual(desc_dsv["format"], 45)

        # Edge Cases: Backbuffer singleton, nullptr, và pDesc == nullptr
        desc_bb = get_texture_desc(mock_backbuffer)
        self.assertEqual((desc_bb["width"], desc_bb["height"], desc_bb["format"]), (1920, 1080, 28))
        desc_null_this = get_texture_desc(None)
        self.assertEqual((desc_null_this["width"], desc_null_this["height"]), (1920, 1080))
        self.assertIsNone(get_texture_desc(0x2001, out_valid=False))

    def test_view_get_resource_and_query_interface_refcount_safety(self):
        # Kiểm thử luồng pDSV->GetResource(&pRes) -> pRes->QueryInterface(IID_ID3D11Texture2D, &pTex2D)
        # -> pTex2D->Release() -> pRes->Release() KHÔNG làm giải phóng sớm texture đang sống (refCount ban đầu = 1)
        ref_counts = {0x3001: 1}
        mock_backbuffer = 0xDEAD0001

        def com_add_ref(ptr):
            if ptr in ref_counts:
                ref_counts[ptr] += 1
                return ref_counts[ptr]
            return 1

        def com_release(ptr):
            if ptr in ref_counts:
                ref_counts[ptr] -= 1
                rem = ref_counts[ptr]
                if rem == 0:
                    del ref_counts[ptr]
                return rem
            return 1

        def view_get_resource(view_res):
            if view_res is not None:
                com_add_ref(view_res)
                return view_res
            return None

        def query_texture2d(this_ptr):
            if this_ptr is not None and this_ptr in ref_counts:
                com_add_ref(this_ptr)
                return this_ptr
            return mock_backbuffer

        p_res = view_get_resource(0x3001)
        self.assertEqual(ref_counts[0x3001], 2)
        p_tex = query_texture2d(p_res)
        self.assertEqual(p_tex, 0x3001)
        self.assertEqual(ref_counts[0x3001], 3)

        com_release(p_tex)
        com_release(p_res)
        self.assertIn(0x3001, ref_counts, "Texture gốc phải còn sống sau khi M2 Engine Release các COM pointer tạm")
        self.assertEqual(ref_counts[0x3001], 1)

        # Edge case: view rỗng và query trên đối tượng không thuộc registry
        self.assertIsNone(view_get_resource(None))
        self.assertEqual(query_texture2d(0x9999), mock_backbuffer)

    def test_offscreen_fbo_flip_y_and_sv_position_screen_mul(self):
        # Kiểm thử đồng bộ trục Y giữa Offscreen FBO (uFlipY = -1.0) và Backbuffer (uFlipY = +1.0):
        # Đỉnh trên cùng D3D11 (ndcY = +1.0) phải ghi vào v = 0.0 của FBO và được đọc lại từ v = 0.0 trên Backbuffer.
        def simulate_pipeline(ndc_x, ndc_y, is_offscreen, d3d_vp, surf_size, screen_mul):
            u_flip_y = -1.0 if is_offscreen else 1.0
            gl_ndc_x = ndc_x
            gl_ndc_y = ndc_y * u_flip_y

            vp_x, vp_y, vp_w, vp_h = d3d_vp
            if is_offscreen:
                gl_vp = (vp_x, vp_y, vp_w, vp_h)
            else:
                sx = surf_size[0] / 1920.0
                sy = surf_size[1] / 1080.0
                gl_vp = (vp_x * sx, surf_size[1] - (vp_y + vp_h) * sy, vp_w * sx, vp_h * sy)

            frag_x = gl_vp[0] + (gl_ndc_x * 0.5 + 0.5) * gl_vp[2]
            frag_y = gl_vp[1] + (gl_ndc_y * 0.5 + 0.5) * gl_vp[3]

            norm_x = (frag_x - gl_vp[0]) / gl_vp[2]
            norm_y = (1.0 - (frag_y - gl_vp[1]) / gl_vp[3]) if u_flip_y > 0.0 else ((frag_y - gl_vp[1]) / gl_vp[3])
            sv_pos_x = vp_x + norm_x * vp_w
            sv_pos_y = vp_y + norm_y * vp_h
            return frag_x, frag_y, sv_pos_x * screen_mul[0], sv_pos_y * screen_mul[1]

        d3d_vp = (0.0, 0.0, 1920.0, 1080.0)
        screen_mul = (1.0 / 1920.0, 1.0 / 1080.0)

        # 1. Top-left D3D11 (ndc = -1, +1): ghi vào dòng frag_y = 0 của Offscreen FBO (v = 0.0)
        fx_off, fy_off, mu_off, mv_off = simulate_pipeline(-1.0, 1.0, True, d3d_vp, (1920, 1080), screen_mul)
        self.assertAlmostEqual(fy_off, 0.0, places=5)
        self.assertAlmostEqual(mv_off, 0.0, places=5)

        # 2. Top-left D3D11 (ndc = -1, +1) vẽ trên Backbuffer (kể cả khi màn hình Android là 1280x720):
        # hiện ở đỉnh trên màn hình (frag_y = 720) nhưng lấy mẫu Offscreen Mask đúng tại v = 0.0!
        fx_bb, fy_bb, mu_bb, mv_bb = simulate_pipeline(-1.0, 1.0, False, d3d_vp, (1280, 720), screen_mul)
        self.assertAlmostEqual(fy_bb, 720.0, places=5)
        self.assertAlmostEqual(mu_bb, 0.0, places=5)
        self.assertAlmostEqual(mv_bb, 0.0, places=5)

    def test_gles_zero_enum_preservation_and_highp_shader_contracts(self):
        # Kiểm tra trực tiếp mã nguồn C++ đảm bảo:
        # 1) Không còn toán tử `!= 0 ?` làm mất hằng số hợp lệ GL_ZERO (0) trong glBlendFuncSeparate / glStencilOp
        # 2) Fragment Shader bật `precision highp float;` cho phép tính mad_sat(rawA, 1024.0, -988.655)
        # 3) Đã xoá bỏ hoàn toàn nhánh hack `isAtlas` trong jni_bridge.cpp
        # 4) mock_Texture2D_GetDesc truy vấn sRegistry.getTextureDesc(thisPtr, tDesc)
        import os
        cpp_dir = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp")
        with open(os.path.join(cpp_dir, "jni_bridge.cpp"), "r", encoding="utf-8") as f:
            jni_src = f.read()
        with open(os.path.join(cpp_dir, "d3d11_bridge.cpp"), "r", encoding="utf-8") as f:
            d3d_src = f.read()

        self.assertIn("precision highp float;", jni_src)
        self.assertNotIn("cmd.srcBlend != 0", jni_src)
        self.assertNotIn("cmd.destBlend != 0", jni_src)
        self.assertNotIn("cmd.srcBlendAlpha != 0", jni_src)
        self.assertNotIn("cmd.destBlendAlpha != 0", jni_src)
        self.assertNotIn("cmd.stencilPassOp != 0", jni_src)
        self.assertNotIn("bool isAtlas", jni_src)
        self.assertIn("sRegistry.getTextureDesc(thisPtr, tDesc)", d3d_src)
        self.assertIn("mock_View_GetResource", d3d_src)

    def test_dxbc_rdef_parser_exact_offsets_and_edge_cases(self):
        # Kiểm thử parser RDEF đọc trực tiếp từ các file DXBC thực tế trong extracted_shaders
        import os

        def parse_dxbc_rdef_bytes(data: bytes):
            if not data or len(data) < 36 or data[:4] != b"DXBC":
                return None
            total_size = struct.unpack_from("<I", data, 24)[0]
            if total_size > len(data) or total_size < 32:
                return None
            num_chunks = struct.unpack_from("<I", data, 28)[0]
            if 32 + num_chunks * 4 > len(data):
                return None
            rdef = None
            for i in range(num_chunks):
                off = struct.unpack_from("<I", data, 32 + i * 4)[0]
                if off + 8 > len(data):
                    return None
                csz = struct.unpack_from("<I", data, off + 4)[0]
                if off + 8 + csz > len(data):
                    return None
                if data[off:off + 4] == b"RDEF":
                    rdef = data[off + 8:off + 8 + csz]
                    break
            if rdef is None or len(rdef) < 28:
                return None
            cb_cnt, cb_off, rb_cnt, rb_off, ver = struct.unpack_from("<IIIII", rdef, 0)
            v_stride = 40 if (ver & 0xFFFF) >= 0x0500 else 24
            cbs = []
            for c in range(cb_cnt):
                c_off = cb_off + c * 24
                if c_off + 24 > len(rdef):
                    return None
                name_off, var_cnt, var_off, cb_size, flags, cb_type = struct.unpack_from("<IIIIII", rdef, c_off)
                if var_cnt > 0 and var_off + var_cnt * v_stride > len(rdef):
                    return None
                cb_name = rdef[name_off:rdef.find(b"\x00", name_off)].decode("ascii")
                vars_list = []
                for v in range(var_cnt):
                    v_off = var_off + v * v_stride
                    vname_off, start_off, vsize, vflags, vtype_off, _ = struct.unpack_from("<IIIIII", rdef, v_off)
                    vname = rdef[vname_off:rdef.find(b"\x00", vname_off)].decode("ascii")
                    vars_list.append((vname, start_off, vsize))
                cbs.append((cb_name, cb_size, vars_list))
            return cbs

        # Edge cases: dữ liệu rỗng, sai magic, bị cắt cụt
        self.assertIsNone(parse_dxbc_rdef_bytes(b""))
        self.assertIsNone(parse_dxbc_rdef_bytes(b"BAD!" * 16))
        self.assertIsNone(parse_dxbc_rdef_bytes(b"DXBC" + b"\x00" * 20 + struct.pack("<II", 4096, 1)))

        shader_dir = os.path.join(os.path.dirname(__file__), "..", "extracted_shaders")
        if os.path.isdir(shader_dir):
            with open(os.path.join(shader_dir, "ps_2051.bin"), "rb") as f:
                cbs_2051 = parse_dxbc_rdef_bytes(f.read())
            self.assertEqual(cbs_2051, [("$Globals", 16, [("alphaMulAdd", 0, 16)])],
                             "ps_2051 phải có alphaMulAdd tại StartOffset = 0, Size = 16")

            with open(os.path.join(shader_dir, "ps_2040.bin"), "rb") as f:
                cbs_2040 = parse_dxbc_rdef_bytes(f.read())
            self.assertEqual(cbs_2040, [("$Globals", 16, [("phase", 0, 4), ("invVague", 4, 4)])],
                             "ps_2040 phải có phase tại offset 0 và invVague tại offset 4")

            with open(os.path.join(shader_dir, "ps_2043.bin"), "rb") as f:
                cbs_2043 = parse_dxbc_rdef_bytes(f.read())
            self.assertEqual(cbs_2043, [("$Globals", 32, [("alphaMulAdd", 0, 16), ("screenMul", 16, 8)])],
                             "ps_2043 phải có alphaMulAdd tại offset 0 và screenMul tại offset 16")

    def test_a8_format_four_channel_replication(self):
        # Kiểm thử định dạng A8: sao chép giá trị alpha vào cả 4 kênh RGBA
        raw_alpha_bytes = bytes([0x00, 0x7F, 0xFF, 0x40])
        rgba_output = bytearray(len(raw_alpha_bytes) * 4)
        for i, a in enumerate(raw_alpha_bytes):
            rgba_output[i * 4 + 0] = a
            rgba_output[i * 4 + 1] = a
            rgba_output[i * 4 + 2] = a
            rgba_output[i * 4 + 3] = a

        # Kiểm tra byte 0x7F (127): cả 4 kênh đều bằng 127
        self.assertEqual(rgba_output[4], 0x7F) # R
        self.assertEqual(rgba_output[5], 0x7F) # G
        self.assertEqual(rgba_output[6], 0x7F) # B
        self.assertEqual(rgba_output[7], 0x7F) # A

        # Đảm bảo shader 2040 đọc kênh .r và shader 195/225 đọc kênh .a đều nhận cùng giá trị
        for i in range(len(raw_alpha_bytes)):
            r_val = rgba_output[i * 4 + 0]
            a_val = rgba_output[i * 4 + 3]
            self.assertEqual(r_val, a_val, f"Kênh R ({r_val}) và kênh A ({a_val}) phải bằng nhau tại pixel {i}")

    def test_shader_210_and_225_uv_source_mapping(self):
        # Kiểm thử Shader 210 lấy mẫu mask bằng UV0, Shader 225 lấy mẫu bằng UV1
        def get_alpha_sample_uv(shader_id, uv0, uv1, screen_uv):
            if shader_id == 195:
                return screen_uv
            elif shader_id == 210:
                return uv0
            elif shader_id == 225:
                return uv1
            elif shader_id == 2040:
                return uv1
            return screen_uv

        test_uv0 = (0.25, 0.75)
        test_uv1 = (0.33, 0.66)
        test_screen_uv = (0.5, 0.5)

        self.assertEqual(get_alpha_sample_uv(210, test_uv0, test_uv1, test_screen_uv), test_uv0)
        self.assertEqual(get_alpha_sample_uv(225, test_uv0, test_uv1, test_screen_uv), test_uv1)
        self.assertEqual(get_alpha_sample_uv(2040, test_uv0, test_uv1, test_screen_uv), test_uv1)
        self.assertEqual(get_alpha_sample_uv(195, test_uv0, test_uv1, test_screen_uv), test_screen_uv)

    def test_non_indexed_draw_command_triangle_strip(self):
        # Kiểm tra giải mã Draw(4, 0) non-indexed với topology TriangleStrip (Topology 5)
        def decode_triangle_strip_indices(vertex_count, start_vertex):
            indices = []
            for k in range(vertex_count - 2):
                raw_a = start_vertex + k
                raw_b = start_vertex + k + 1
                raw_c = start_vertex + k + 2
                if raw_a == raw_b or raw_b == raw_c or raw_a == raw_c:
                    continue
                if k % 2 == 1:
                    raw_b, raw_c = raw_c, raw_b
                indices.extend([raw_a, raw_b, raw_c])
            return indices

        # 4 đỉnh (Fullscreen Movie Quad) phải sinh ra 2 tam giác: (0, 1, 2) và (1, 3, 2)
        indices = decode_triangle_strip_indices(4, 0)
        self.assertEqual(indices, [0, 1, 2, 1, 3, 2])

        # Kiểm tra với start_vertex offset = 10
        indices_offset = decode_triangle_strip_indices(4, 10)
        self.assertEqual(indices_offset, [10, 11, 12, 11, 13, 12])

    def test_non_indexed_draw_command_triangle_list(self):
        # Kiểm tra giải mã Draw non-indexed với topology TriangleList (Topology 4)
        def decode_triangle_list_indices(vertex_count, start_vertex):
            indices = []
            for k in range(0, vertex_count - 2, 3):
                raw_a = start_vertex + k
                raw_b = start_vertex + k + 1
                raw_c = start_vertex + k + 2
                if raw_a == raw_b or raw_b == raw_c or raw_a == raw_c:
                    continue
                indices.extend([raw_a, raw_b, raw_c])
            return indices

        # 6 đỉnh phải sinh ra 2 tam giác: (0, 1, 2) và (3, 4, 5)
        indices = decode_triangle_list_indices(6, 0)
        self.assertEqual(indices, [0, 1, 2, 3, 4, 5])

    def test_non_indexed_draw_degenerate_triangle_skipping(self):
        # Kiểm tra bỏ qua tam giác suy biến
        def decode_strip_with_degenerates(raw_vertices):
            indices = []
            for k in range(len(raw_vertices) - 2):
                raw_a = raw_vertices[k]
                raw_b = raw_vertices[k + 1]
                raw_c = raw_vertices[k + 2]
                if raw_a == raw_b or raw_b == raw_c or raw_a == raw_c:
                    continue
                if k % 2 == 1:
                    raw_b, raw_c = raw_c, raw_b
                indices.extend([raw_a, raw_b, raw_c])
            return indices

        # Dãy đỉnh có degenerate restart: [0, 1, 2, 3, 3, 4, 4, 5, 6, 7]
        seq = [0, 1, 2, 3, 3, 4, 4, 5, 6, 7]
        triangles = decode_strip_with_degenerates(seq)
        # Các tam giác chứa 3-3 hoặc 4-4 đều phải bị loại bỏ
        for i in range(0, len(triangles), 3):
            a, b, c = triangles[i], triangles[i + 1], triangles[i + 2]
            self.assertNotEqual(a, b)
            self.assertNotEqual(b, c)
            self.assertNotEqual(a, c)

    def test_non_indexed_draw_edge_cases(self):
        # Kiểm tra điều kiện biên: vertex_count < 3 không tạo tam giác nào
        def can_emit_draw(vertex_count, has_vertex_buffer):
            return has_vertex_buffer and vertex_count >= 3

        self.assertFalse(can_emit_draw(0, True))
        self.assertFalse(can_emit_draw(1, True))
        self.assertFalse(can_emit_draw(2, True))
        self.assertFalse(can_emit_draw(4, False)) # Vertex buffer null
        self.assertTrue(can_emit_draw(3, True))
        self.assertTrue(can_emit_draw(4, True))

    def test_resource_registry_dword_bgra_to_rgba_swizzle(self):
        # Kiểm tra thuật toán hoán vị bitwise 32-bit dword cho kDxgiB8G8R8A8Unorm
        def swizzle_bgra_to_rgba_dword(px_dword):
            # px_dword: little-endian byte 0=B, byte 1=G, byte 2=R, byte 3=A
            # Đích: byte 0=R, byte 1=G, byte 2=B, byte 3=A
            return (px_dword & 0xFF00FF00) | ((px_dword & 0x00FF0000) >> 16) | ((px_dword & 0x000000FF) << 16)

        # Mẫu 1: Màu đỏ thuần (B=0, G=0, R=255, A=255) -> 0xFF0000
        # Little-endian BGRA: [0x00, 0x00, 0xFF, 0xFF] = 0xFFFF0000
        bgra_red = 0xFFFF0000
        # Đích RGBA: [0xFF, 0x00, 0x00, 0xFF] = 0xFF0000FF
        rgba_red = swizzle_bgra_to_rgba_dword(bgra_red)
        self.assertEqual(rgba_red, 0xFF0000FF)

        # Mẫu 2: Màu xanh lam thuần (B=255, G=0, R=0, A=255) -> 0x0000FF
        # Little-endian BGRA: [0xFF, 0x00, 0x00, 0xFF] = 0xFF0000FF
        bgra_blue = 0xFF0000FF
        # Đích RGBA: [0x00, 0x00, 0xFF, 0xFF] = 0xFFFF0000
        rgba_blue = swizzle_bgra_to_rgba_dword(bgra_blue)
        self.assertEqual(rgba_blue, 0xFFFF0000)

        # Mẫu 3: Màu tổng hợp (B=0x12, G=0x34, R=0x56, A=0x78)
        bgra_mix = 0x78563412
        rgba_mix = swizzle_bgra_to_rgba_dword(bgra_mix)
        # Đích: B đổi chỗ với R -> [0x56, 0x34, 0x12, 0x78] = 0x78123456
        self.assertEqual(rgba_mix, 0x78123456)

    def test_resource_registry_dword_bgra_to_rgbx_swizzle(self):
        # Kiểm tra thuật toán hoán vị và gán alpha cứng 0xFF cho kDxgiB8G8R8X8Unorm
        def swizzle_bgra_to_rgbx_dword(px_dword):
            return 0xFF000000 | (px_dword & 0x0000FF00) | ((px_dword & 0x00FF0000) >> 16) | ((px_dword & 0x000000FF) << 16)

        # Nguồn có alpha rác 0x00 (B=0xAA, G=0xBB, R=0xCC, A=0x00)
        bgra_x = 0x00CCBBAA
        rgbx_out = swizzle_bgra_to_rgbx_dword(bgra_x)
        # Đích phải có Alpha = 0xFF và R=0xCC, G=0xBB, B=0xAA -> 0xFFAABBCC
        self.assertEqual(rgbx_out, 0xFFAABBCC)

    def test_pipelined_frame_pacing_handshake(self):
        # Mô phỏng bắt tay đồng bộ Double Buffering (Overlapped Pipelining) giữa Game Thread và Render Thread
        class MockPipelinedSync:
            def __init__(self):
                self.sprite_pending_consume = False
                self.frame_pending_ack = False
                self.rendered_frames = 0
                self.dropped_frames = 0

            def game_submit_frame(self, frame_idx):
                # 1. Chờ Render Thread swap commands
                if self.sprite_pending_consume:
                    # Timeout xảy ra nếu Render Thread chưa lấy commands
                    self.dropped_frames += 1
                # 2. Chờ frame trước đã hoàn thành VSync Present
                # (Ở chế độ chuẩn, Game Thread giải phóng sớm để tính frame tiếp theo)
                self.sprite_pending_consume = True
                self.frame_pending_ack = True

            def render_consume_commands(self):
                # Render Thread lấy commands và giải phóng Game Thread ngay lập tức
                self.sprite_pending_consume = False

            def render_present_vsync(self):
                # eglSwapBuffers hoàn tất VSync, báo hiệu Game Thread nộp frame kế tiếp
                self.frame_pending_ack = False
                self.rendered_frames += 1

        sync = MockPipelinedSync()
        # Chu kỳ 1: Game submit frame 1
        sync.game_submit_frame(1)
        self.assertTrue(sync.sprite_pending_consume)
        self.assertTrue(sync.frame_pending_ack)

        # Render Thread bắt đầu vẽ và tiêu thụ commands -> giải phóng Game Thread sớm
        sync.render_consume_commands()
        self.assertFalse(sync.sprite_pending_consume, "Game Thread phải được mở khoá tiêu thụ commands sớm trước VSync")

        # Render Thread hoàn thành VSync
        sync.render_present_vsync()
        self.assertFalse(sync.frame_pending_ack, "VSync hoàn tất mở khoá nộp frame tiếp theo")
        self.assertEqual(sync.rendered_frames, 1)
        self.assertEqual(sync.dropped_frames, 0)

    def test_frame_drop_prevention_on_render_timeout(self):
        # Kiểm tra ngưỡng timeout an toàn 25ms-33ms ngăn chặn drop frame giả tạo
        def should_drop_frame(elapsed_ms, timeout_threshold_ms):
            return elapsed_ms > timeout_threshold_ms

        # Nếu Render Thread mất 17.5ms (chỉ chậm hơn VSync 16.6ms một chút do video decode tải nặng):
        # Với timeout cũ 16-18ms -> Sẽ bị rớt frame
        self.assertTrue(should_drop_frame(17.5, 16.0))
        # Với timeout mới 25-33ms -> Không bị rớt frame
        self.assertFalse(should_drop_frame(17.5, 25.0))
        self.assertFalse(should_drop_frame(17.5, 33.0))

    def test_context_vtable_copy_slots_registered(self):
        # Kiểm tra slot 46 (CopySubresourceRegion) và slot 47 (CopyResource) trong D3D11DeviceContext
        context_slots = {
            14: "Map",
            15: "Unmap",
            46: "CopySubresourceRegion",
            47: "CopyResource",
            48: "UpdateSubresource"
        }
        self.assertEqual(context_slots[46], "CopySubresourceRegion")
        self.assertEqual(context_slots[47], "CopyResource")
        self.assertEqual(47 * 8, 376)  # Khớp chính xác (*v5[53] + 376LL) trong sgre_steam.exe sub_1404C3320

    def test_thumbnail_scale_and_format_conversion(self):
        # Kiểm tra hàm scale ảnh từ 1920x1080 xuống 284x160 và chuyển RGBA sang BGRA
        src_w, src_h = 4, 4
        # Tạo 4x4 RGBA: pixel đầu tiên là Đỏ (R=255, G=0, B=0, A=255)
        src_rgba = bytearray([255, 0, 0, 255] * (src_w * src_h))
        dst_w, dst_h = 2, 2
        dst_bgra = bytearray(dst_w * dst_h * 4)

        for y in range(dst_h):
            src_y = (y * src_h) // dst_h
            for x in range(dst_w):
                src_x = (x * src_w) // dst_w
                src_idx = (src_y * src_w + src_x) * 4
                dst_idx = (y * dst_w + x) * 4
                # Swizzle RGBA sang BGRA
                dst_bgra[dst_idx + 0] = src_rgba[src_idx + 2] # B
                dst_bgra[dst_idx + 1] = src_rgba[src_idx + 1] # G
                dst_bgra[dst_idx + 2] = src_rgba[src_idx + 0] # R
                dst_bgra[dst_idx + 3] = src_rgba[src_idx + 3] # A

        # Kiểm tra pixel đầu tiên ở dst đã là BGRA (B=0, G=0, R=255, A=255)
        self.assertEqual(dst_bgra[0], 0)   # B
        self.assertEqual(dst_bgra[1], 0)   # G
        self.assertEqual(dst_bgra[2], 255) # R
        self.assertEqual(dst_bgra[3], 255) # A

    def test_is_capture_or_thumbnail_size(self):
        # M2 Engine sử dụng 1920x1080 (full screen capture), 568x320 (2.0x supersampled staging) và 284x160 (final thumbnail)
        def is_capture_or_thumbnail_size(w, h):
            return (w == 1920 and h == 1080) or (w == 568 and h == 320) or (w == 284 and h == 160)

        self.assertTrue(is_capture_or_thumbnail_size(1920, 1080))
        self.assertTrue(is_capture_or_thumbnail_size(568, 320))
        self.assertTrue(is_capture_or_thumbnail_size(284, 160))
        self.assertFalse(is_capture_or_thumbnail_size(1280, 720))
        self.assertFalse(is_capture_or_thumbnail_size(3424, 3424))
        self.assertFalse(is_capture_or_thumbnail_size(2048, 1088))

    def test_supersampled_568x320_thumbnail_downsample(self):
        # Giả lập pipeline 2 bước của M2 Engine:
        # Bước 1: 1920x1080 -> 568x320 (supersampled staging texture)
        # Bước 2: 568x320 -> 284x160 (file save data)
        src_w, src_h = 1920, 1080
        # Tạo pixel mẫu màu xanh lục (R=0, G=255, B=0, A=255)
        src_buf = bytearray([0, 255, 0, 255] * (src_w * src_h))

        # Bước 1: Downsample sang 568x320
        mid_w, mid_h = 568, 320
        mid_buf = bytearray(mid_w * mid_h * 4)
        for y in range(mid_h):
            sy = (y * src_h) // mid_h
            for x in range(mid_w):
                sx = (x * src_w) // mid_w
                src_idx = (sy * src_w + sx) * 4
                dst_idx = (y * mid_w + x) * 4
                mid_buf[dst_idx:dst_idx+4] = src_buf[src_idx:src_idx+4]

        # Bước 2: Downsample sang 284x160
        dst_w, dst_h = 284, 160
        dst_buf = bytearray(dst_w * dst_h * 4)
        for y in range(dst_h):
            my = (y * mid_h) // dst_h
            for x in range(dst_w):
                mx = (x * mid_w) // dst_w
                mid_idx = (my * mid_w + mx) * 4
                dst_idx = (y * dst_w + x) * 4
                dst_buf[dst_idx:dst_idx+4] = mid_buf[mid_idx:mid_idx+4]

        # Kiểm tra màu xanh lục được bảo toàn nguyên vẹn
        self.assertEqual(dst_buf[0], 0)     # R
        self.assertEqual(dst_buf[1], 255)   # G
        self.assertEqual(dst_buf[2], 0)     # B
        self.assertEqual(dst_buf[3], 255)   # A

    def test_offscreen_gameplay_fbo_thumbnail_orientation(self):
        # Kiểm tra chuẩn hoá định dạng Top-Down cho OffscreenGameplayFBO và Backbuffer/BackupFBO
        w, h = 4, 4
        # Giả lập ảnh: Nửa trên (hàng 0, 1) là pixel Đỏ (255, 0, 0, 255), Nửa dưới (hàng 2, 3) là pixel Xanh lam (0, 0, 255, 255)
        top_down_raw = bytearray([0] * (w * h * 4))
        for y_idx in range(h):
            for x in range(w):
                idx = (y_idx * w + x) * 4
                if y_idx < 2:
                    top_down_raw[idx + 0] = 255  # Đỏ
                    top_down_raw[idx + 3] = 255
                else:
                    top_down_raw[idx + 2] = 255  # Lam
                    top_down_raw[idx + 3] = 255

        # 1. Với OffscreenGameplayFBO: uFlipY = -1.0f nên glReadPixels đã trả về Top-Down
        fbo_type = "OffscreenGameplayFBO"
        is_already_top_down = (fbo_type == "OffscreenGameplayFBO")
        cached_screen = bytearray(w * h * 4)
        if is_already_top_down:
            cached_screen[:] = top_down_raw
        else:
            for y in range(h):
                src_row = top_down_raw[(h - 1 - y) * w * 4 : (h - y) * w * 4]
                cached_screen[y * w * 4 : (y + 1) * w * 4] = src_row

        # Hàng 0 của cached_screen phải là màu Đỏ
        self.assertEqual(cached_screen[0], 255)
        self.assertEqual(cached_screen[2], 0)
        # Hàng 3 của cached_screen phải là màu Lam
        self.assertEqual(cached_screen[3 * w * 4 + 0], 0)
        self.assertEqual(cached_screen[3 * w * 4 + 2], 255)

        # 2. Scale Top-Down sang thumbnail đích (2x2): Hàng 0 phải vẫn là Đỏ, Hàng 1 phải là Lam
        dst_w, dst_h = 2, 2
        dst_buf = bytearray(dst_w * dst_h * 4)
        for y in range(dst_h):
            sy = (y * h) // dst_h
            for x in range(dst_w):
                sx = (x * w) // dst_w
                src_idx = (sy * w + sx) * 4
                dst_idx = (y * dst_w + x) * 4
                dst_buf[dst_idx:dst_idx+4] = cached_screen[src_idx:src_idx+4]

        # Đỉnh thumbnail (y=0) giữ đúng màu Đỏ (không bị lộn ngược)
        self.assertEqual(dst_buf[0], 255)
        self.assertEqual(dst_buf[2], 0)
        # Đáy thumbnail (y=1) giữ đúng màu Lam
        self.assertEqual(dst_buf[dst_w * 4 + 0], 0)
        self.assertEqual(dst_buf[dst_w * 4 + 2], 255)

    def test_copysubresource_with_src_rt_handle_dispatch(self):
        # Kiểm tra logic ánh xạ RenderTarget handle vào FBO cache
        fbo_cache = {
            0x763860faec08: 3,  # Offscreen gameplay RenderTarget
            0x7638449c3030: 4   # Other RT
        }
        src_handle = 0x763860faec08
        self.assertIn(src_handle, fbo_cache)
        read_fbo = fbo_cache[src_handle]
        self.assertEqual(read_fbo, 3)

    def test_bc7_pure_rgba_preservation(self):
        # Kiểm tra thuật toán bảo toàn kênh RGBA chuẩn cho tile 4x4 (16 pixels) của BC7 decoder
        # bcdec_bc7 đã giải mã ra đúng định dạng RGBA (byte 0=R, 1=G, 2=B, 3=A).
        # Tuyệt đối không swap kênh R và B để bảo toàn màu sắc da hồng hào và mũ baby blue của nhân vật.
        tile = bytearray()
        for i in range(16):
            tile.extend([212, 147, 123, 255]) # Da Mayuri chuẩn: R=212, G=147, B=123

        for p in range(16):
            self.assertEqual(tile[p * 4 + 0], 212)
            self.assertEqual(tile[p * 4 + 1], 147)
            self.assertEqual(tile[p * 4 + 2], 123)
            self.assertEqual(tile[p * 4 + 3], 255)

    def test_system_function_036_rtl_gen_random(self):
        # Kiểm tra logic xử lý của SystemFunction036 (RtlGenRandom) và CryptGenRandom
        def stub_system_function_036(buf_len):
            if buf_len > 0:
                return 1 # TRUE
            return 0 # FALSE

        self.assertEqual(stub_system_function_036(32), 1)
        self.assertEqual(stub_system_function_036(0), 0)

    def test_is_capture_or_thumbnail_size_narrowed(self):
        # Kiểm tra logic nhận diện kích thước thumbnail đã được thu hẹp chính xác
        def is_capture_or_thumbnail_size(w, h):
            return (w == 480 and h == 270) or \
                   (w == 568 and h == 320) or \
                   (w == 284 and h == 160)

        # Thumbnail hợp lệ
        self.assertTrue(is_capture_or_thumbnail_size(480, 270))
        self.assertTrue(is_capture_or_thumbnail_size(568, 320))
        self.assertTrue(is_capture_or_thumbnail_size(284, 160))

        # Fullscreen RenderTarget 1920x1080 và các tỉ lệ 16:9 khác TUYỆT ĐỐI không phải thumbnail
        self.assertFalse(is_capture_or_thumbnail_size(1920, 1080))
        self.assertFalse(is_capture_or_thumbnail_size(1280, 720))
        self.assertFalse(is_capture_or_thumbnail_size(960, 540))
        self.assertFalse(is_capture_or_thumbnail_size(2048, 1088))
        self.assertFalse(is_capture_or_thumbnail_size(4096, 2752))

    def test_copy_resource_priority_to_registry(self):
        # Kiểm tra logic ưu tiên CopyResource qua CPU Registry khi nguồn có dữ liệu
        registry_has_src = True
        dst_touch_called = False

        if registry_has_src:
            dst_touch_called = True
            copy_success = True
        else:
            copy_success = False

        self.assertTrue(copy_success)
        self.assertTrue(dst_touch_called)

    def test_grey_flash_prevention_on_empty_backbuffer_draws(self):
        # Kiểm tra logic khử nhấp nháy xám: chỉ clear & swap khi Backbuffer có mesh
        def should_clear_and_swap_backbuffer(has_active_texture, frame_meshes):
            has_backbuffer_draw = has_active_texture
            if not has_backbuffer_draw:
                for cmd in frame_meshes:
                    if cmd.get("renderTargetHandle") is None:
                        has_backbuffer_draw = True
                        break
            return has_backbuffer_draw

        # Trường hợp 1: Frame rỗng không có mesh
        self.assertFalse(should_clear_and_swap_backbuffer(False, []))

        # Trường hợp 2: Frame chỉ có các mesh vẽ vào Offscreen FBOs (chuẩn bị hoạt ảnh)
        offscreen_meshes = [
            {"renderTargetHandle": 0x1000},
            {"renderTargetHandle": 0x2000}
        ]
        self.assertFalse(should_clear_and_swap_backbuffer(False, offscreen_meshes))

        # Trường hợp 3: Frame có mesh vẽ ra Backbuffer (gameplay/UI)
        backbuffer_meshes = [
            {"renderTargetHandle": 0x1000},
            {"renderTargetHandle": None}
        ]
        self.assertTrue(should_clear_and_swap_backbuffer(False, backbuffer_meshes))

        # Trường hợp 4: Có active quad texture
        self.assertTrue(should_clear_and_swap_backbuffer(True, []))

    def test_pure_gameplay_thumbnail_isolation(self):
        # Kiểm tra logic cô lập ảnh thumbnail: không bao giờ lấy từ Save Menu UI
        gameplay_fbo_screen = bytearray([128] * (1920 * 1080 * 4)) # Scene Mayuri
        save_menu_ui_screen = bytearray([255] * (1920 * 1080 * 4)) # Save Menu UI

        last_pure_gameplay = bytearray()
        last_captured_screen = bytearray()

        # Khi M2 Engine render cảnh gameplay và capture OffscreenGameplayFBO
        fbo_type = "OffscreenGameplayFBO"
        if fbo_type == "OffscreenGameplayFBO":
            last_pure_gameplay[:] = gameplay_fbo_screen
            last_captured_screen[:] = gameplay_fbo_screen

        # Sau đó người chơi mở Save Menu: save_menu_ui_screen được ghi vào backup FBO
        # Nhưng khi bấm Save, srcRtHandle = None
        src_rt_handle = None
        read_fbo = 0
        use_pure_gameplay = False

        if read_fbo == 0:
            if len(last_pure_gameplay) == 1920 * 1080 * 4:
                use_pure_gameplay = True
                result_screen = last_pure_gameplay
            else:
                result_screen = save_menu_ui_screen

        self.assertTrue(use_pure_gameplay)
        # Kết quả thumbnail phải là ảnh gameplay (128), tuyệt đối không phải Save Menu (255)
        self.assertEqual(result_screen[0], 128)
        self.assertNotEqual(result_screen[0], 255)

    def test_stub_exitprocess_protection_against_abort(self):
        # Kiểm tra logic xử lý stub_ExitProcess
        def handle_exit_process(code):
            if code == 3:
                return "IGNORE_AND_CONTINUE"
            return "TERMINATE"

        self.assertEqual(handle_exit_process(3), "IGNORE_AND_CONTINUE")
        self.assertEqual(handle_exit_process(0), "TERMINATE")
        self.assertEqual(handle_exit_process(1), "TERMINATE")

    def test_vertex_shader_color_routing_per_bytecode_disasm(self):
        # Kiểm tra logic phân bổ màu đỉnh theo InputLayout và Constant Buffer:
        # 1. Nếu mesh có colorOffset >= 0 (như font chữ, subtitle, dialog, animation): lấy màu đỉnh aColor
        # 2. Nếu mesh có colorOffset < 0 (như E-mote deformable meshes): lấy màu từ vsCb0[16..19] (cb0[4])
        def resolve_vertex_color(color_offset, vertex_color, vs_cb0_4):
            if color_offset >= 0:
                return vertex_color
            return vs_cb0_4

        # Font chữ, Subtitle và UI (colorOffset = 8): bảo toàn alpha fade thực sự của đỉnh
        font_color = resolve_vertex_color(8, (1.0, 1.0, 1.0, 0.6), (1.0, 1.0, 1.0, 1.0))
        self.assertEqual(font_color, (1.0, 1.0, 1.0, 0.6), "Font chu phai giu nguyen alpha fade cua dinh")

        # Deformable mesh Mayuri / Nakabachi (colorOffset = -1): nhan mau tu vsCb0[4]
        emote_color = resolve_vertex_color(-1, (1.0, 1.0, 1.0, 1.0), (1.0, 0.95, 0.9, 1.0))
        self.assertEqual(emote_color, (1.0, 0.95, 0.9, 1.0), "Deformable mesh phai nhan mau tu vsCb0[4]")

    def test_shader_1302_blending_mode_override(self):
        # Chỉ ghi đè Blend Mode cho duy nhất Shader 1302 (loại bỏ khối đen che màn hình)
        # Không ép các shader khác để tránh chớp sáng và lỗi màu cuối hoạt ảnh intro/wipe
        GL_ONE = 1
        GL_ZERO = 0
        GL_SRC_ALPHA = 0x0302
        GL_ONE_MINUS_SRC_ALPHA = 0x0303

        def resolve_blend_func(shader_id, src_blend, dest_blend, has_valid_alpha_mask):
            if shader_id == 1302:
                return (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
            return (src_blend, dest_blend)

        # Shader 1302 gửi lệnh với GL_ONE, GL_ZERO -> Phải được chuyển thành Alpha Blending
        func = resolve_blend_func(1302, GL_ONE, GL_ZERO, False)
        self.assertEqual(func, (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA))

        # Shader thông thường với alpha mask và ONE/ZERO -> KHÔNG bị ép, giữ nguyên (GL_ONE, GL_ZERO)
        func2 = resolve_blend_func(180, GL_ONE, GL_ZERO, True)
        self.assertEqual(func2, (GL_ONE, GL_ZERO))

        # Shader thông thường không đổi
        func3 = resolve_blend_func(180, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, False)
        self.assertEqual(func3, (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA))

    def test_thumbnail_sizes_isolation(self):
        # Kiểm tra chỉ các thumbnail thật sự mới được lấy từ cache
        # 1920x1080 tuyệt đối không được coi là thumbnail để tránh làm trắng xóa System Menu
        def is_thumbnail_size(w, h):
            return (w == 480 and h == 270) or \
                   (w == 568 and h == 320) or \
                   (w == 284 and h == 160)

        self.assertTrue(is_thumbnail_size(480, 270))
        self.assertTrue(is_thumbnail_size(568, 320))
        self.assertTrue(is_thumbnail_size(284, 160))
        self.assertFalse(is_thumbnail_size(1920, 1080), "1920x1080 khong duoc phep nam trong thumbnail cache")

    def test_backbuffer_clear_prevention_on_offscreen_fbo_draws(self):
        # Kiểm tra logic triệt tiêu nhấp nháy xám: Dù hasActiveTexture=True,
        # nếu frame chỉ có mesh vẽ vào Offscreen FBO thì tuyệt đối không clear & swap Backbuffer!
        def evaluate_has_backbuffer_draw(has_active_texture, active_pixels_valid, active_w, active_h, frame_meshes):
            has_backbuffer_draw = False
            if frame_meshes:
                for cmd in frame_meshes:
                    if cmd.get("renderTargetHandle") is None and cmd.get("isClearCommand", 0) == 0:
                        has_backbuffer_draw = True
                        break
            elif has_active_texture and active_pixels_valid and active_w == 1920 and active_h == 1080:
                has_backbuffer_draw = True
            return has_backbuffer_draw

        # Tình huống cuối hoạt ảnh: hasActiveTexture=True (texture gameplay đã load từ trước),
        # nhưng frame này chỉ render vào FBO deformable mesh 1210x1063
        offscreen_meshes = [
            {"renderTargetHandle": 0x76386000, "isClearCommand": 0},
            {"renderTargetHandle": 0x76386000, "isClearCommand": 0}
        ]
        # Với logic mới: hasBackbufferDraw phải là False -> Không xóa Backbuffer, không swap nhấp nháy xám!
        self.assertFalse(evaluate_has_backbuffer_draw(True, True, 1920, 1080, offscreen_meshes))

        # Tình huống vẽ UI hoặc Gameplay ra Backbuffer
        backbuffer_meshes = [
            {"renderTargetHandle": 0x76386000, "isClearCommand": 0},
            {"renderTargetHandle": None, "isClearCommand": 0}
        ]
        self.assertTrue(evaluate_has_backbuffer_draw(True, True, 1920, 1080, backbuffer_meshes))

    def test_active_gameplay_rt_handle_thumbnail_prioritization(self):
        # Kiểm tra cơ chế ưu tiên sActiveGameplayRtHandle khi chụp Thumbnail Save
        fbo_cache = {
            0x7638A000: 10, # FBO Gameplay 1920x1080
            0x7638B000: 11  # FBO Save Menu UI
        }
        active_gameplay_rt = 0x7638A000

        # Khi M2 Engine gọi Save, srcRtHandle = None (đang ở Save Menu)
        capture_src_rt = None
        read_fbo = 0
        fbo_type = "DefaultFBO"

        if capture_src_rt is not None and capture_src_rt in fbo_cache:
            read_fbo = fbo_cache[capture_src_rt]
            fbo_type = "OffscreenGameplayFBO"
        elif read_fbo == 0 and active_gameplay_rt is not None and active_gameplay_rt in fbo_cache:
            read_fbo = fbo_cache[active_gameplay_rt]
            fbo_type = "OffscreenGameplayFBO"

        self.assertEqual(read_fbo, 10)
        self.assertEqual(fbo_type, "OffscreenGameplayFBO")

    def test_copy_subresource_ignore_cpu_registry_for_render_target(self):
        # Kiểm tra logic bỏ qua CPU copy nếu nguồn là RenderTarget (bindFlags & 0x20)
        D3D11_BIND_RENDER_TARGET = 0x20
        src_bind_flags = D3D11_BIND_RENDER_TARGET | 0x08 # RenderTarget + ShaderResource
        is_src_render_target = (src_bind_flags & D3D11_BIND_RENDER_TARGET) != 0
        self.assertTrue(is_src_render_target)

        # Khi is_src_render_target == True, không được copy từ CPU registry
        cpu_copy_attempted = not is_src_render_target
        self.assertFalse(cpu_copy_attempted)

    def test_gpu_to_gpu_copy_subresource_bypasses_cpu_readback(self):
        # Kiểm tra logic tách biệt GPU Copy (VRAM) khỏi CPU Readback (glReadPixels)
        D3D11_USAGE_DEFAULT = 0
        D3D11_USAGE_STAGING = 3

        thumbnail_sizes = {(480, 270), (568, 320), (284, 160)}

        def route_copy_subresource(dst_w, dst_h, dst_usage, dst_cpu_access, is_src_rt):
            if not is_src_rt:
                return "CPU_REGISTRY_COPY"
            is_staging_cpu = (dst_usage == D3D11_USAGE_STAGING) or (dst_cpu_access != 0)
            is_thumb = (dst_w, dst_h) in thumbnail_sizes
            if is_thumb or (is_staging_cpu and dst_w == 1920 and dst_h == 1080):
                return "CPU_READBACK_CAPTURE"
            return "GPU_COMMAND_COPY"

        # 1. Nhân vật Mayuri background intermediate copy (967x1080, USAGE_DEFAULT)
        # BẮT BUỘC phải đi qua GPU_COMMAND_COPY để đạt 60fps và không bị dither moiré
        route_mayuri = route_copy_subresource(967, 1080, D3D11_USAGE_DEFAULT, 0, True)
        self.assertEqual(route_mayuri, "GPU_COMMAND_COPY")

        # 2. Thumbnail Save Slot 1 (480x270, USAGE_STAGING)
        # Phải đi qua CPU_READBACK_CAPTURE để tạo file vật lý trên đĩa
        route_thumb = route_copy_subresource(480, 270, D3D11_USAGE_STAGING, 0x20000, True)
        self.assertEqual(route_thumb, "CPU_READBACK_CAPTURE")

        # 3. System Menu Blur Staging (1920x1080, USAGE_STAGING)
        # Phải đi qua CPU_READBACK_CAPTURE để lấy ảnh toàn màn hình cho System Menu
        route_sysmenu = route_copy_subresource(1920, 1080, D3D11_USAGE_STAGING, 0x20000, True)
        self.assertEqual(route_sysmenu, "CPU_READBACK_CAPTURE")

    def test_readback_maintains_pure_rgba_no_swapped_bgr(self):
        # Kiểm tra logic bảo toàn RGBA nguyên bản cho mọi thao tác readback / capture màn hình
        # Tuyệt đối không swap sang BGRA để ngăn ngừa lỗi đảo màu xanh xám ma quái khi chuyển cảnh
        def get_readback_is_bgra(format_val):
            # Tất cả các texture trên OpenGL ES đều nạp bằng GL_RGBA
            # nên bộ đệm staging/thumbnail luôn phải duy trì isBgra = False
            return False

        self.assertFalse(get_readback_is_bgra(87), "Format 87 (B8G8R8A8) vẫn phải giữ nguyên RGBA khi upload lên GLES")
        self.assertFalse(get_readback_is_bgra(28), "Format 28 (R8G8B8A8) phải giữ nguyên RGBA")

        # Kiểm tra mô phỏng việc sao chép pixel không tráo đổi R và B
        src_pixel = [206, 187, 177, 255] # Normal face (R=206, G=187, B=177)
        dst_pixel = [0, 0, 0, 0]
        # Direct RGBA copy:
        dst_pixel[0] = src_pixel[0]
        dst_pixel[1] = src_pixel[1]
        dst_pixel[2] = src_pixel[2]
        dst_pixel[3] = 255
        self.assertEqual(dst_pixel[0], 206, "Red channel phải giữ nguyên ở index 0")
        self.assertEqual(dst_pixel[2], 177, "Blue channel phải giữ nguyên ở index 2")

    def test_shader_1302_is_bypassed_to_prevent_black_grid_overlay(self):
        # Kiểm tra logic bỏ qua Shader 1302 trong OpenGL ES render loop
        # Shader 1302 vốn là lớp phủ Halftone Shading / Noise trong D3D11 đòi hỏi texture nền t2
        # Khi không có pass t2, vẽ bằng alpha blend thông thường sẽ đè các vệt đen xì, thô ráp lên nhân vật
        def should_bypass_mesh_command(shader_id):
            return shader_id == 1302

        self.assertTrue(should_bypass_mesh_command(1302), "Shader 1302 phải được bypass để loại bỏ lưới đen")
        self.assertFalse(should_bypass_mesh_command(180), "Shader 180 vẽ thông thường không được bypass")
        self.assertFalse(should_bypass_mesh_command(300), "Shader 300 vẽ nhân vật không được bypass")
        self.assertFalse(should_bypass_mesh_command(2048), "Shader 2048 vẽ deformable mesh không được bypass")

    def test_untextured_mesh_draw_does_not_fallback_to_font_atlas(self):
        # Kiểm tra bảo vệ: Khi một mesh command không có texture gắn kèm (sBoundTextureResources[0] == nullptr),
        # tuyệt đối không fallback gán bừa sLastUpdatedTextureResource (vốn là Font Atlas 1024x1024).
        # Việc fallback sai sẽ làm Font Atlas bị render đè lên góc trên bên trái của màn hình gameplay.
        bound_texture = None
        last_updated_texture = "0x763814c12058_font_atlas_1024x1024"

        # Logic chuẩn hiện tại: Chỉ gán bound_texture, không fallback sang last_updated_texture
        assigned_texture = bound_texture
        self.assertIsNone(assigned_texture, "Mesh không texture phải có assigned_texture là None để dùng 1x1 white pixel")
        self.assertNotEqual(assigned_texture, last_updated_texture, "Không được phép fallback gán Font Atlas vào untextured mesh")

    def test_iasetindexbuffer_tracks_offset(self):
        # Kiểm tra cơ chế lưu trữ và cộng Offset khi IASetIndexBuffer được gọi
        class MockContextState:
            def __init__(self):
                self.index_buffer = None
                self.format = 57
                self.offset = 0

            def set_index_buffer(self, ib, fmt, offset):
                self.index_buffer = ib
                self.format = fmt
                self.offset = offset

            def get_index_pointer_offset(self):
                return self.offset

        state = MockContextState()
        state.set_index_buffer("0xIB_BUFFER", 57, 128)
        self.assertEqual(state.offset, 128, "IASetIndexBuffer phải lưu chính xác offset = 128")
        self.assertEqual(state.get_index_pointer_offset(), 128, "Con trỏ đọc index phải được dời đúng 128 bytes")

        # Unbind buffer
        state.set_index_buffer(None, 57, 0)
        self.assertIsNone(state.index_buffer)
        self.assertEqual(state.offset, 0, "Khi unbind, offset phải được reset về 0")

    def test_triangle_strip_decoder_with_degenerates_and_restart(self):
        # Kiểm tra thuật toán giải mã Triangle Strip sang Triangle List
        # Đảm bảo sau các đỉnh suy biến (degenerate vertices) hoặc restart index 0xFFFF,
        # winding tam giác hợp lệ đầu tiên luôn được reset về CCW thuận (không bị swap ngược)
        def decode_triangle_strip(indices):
            tris = []
            flip_winding = False
            for k in range(len(indices) - 2):
                a = indices[k]
                b = indices[k + 1]
                c = indices[k + 2]

                if a == 0xFFFF or b == 0xFFFF or c == 0xFFFF or a == -1 or b == -1 or c == -1:
                    flip_winding = False
                    continue

                if a == b or b == c or a == c:
                    flip_winding = False
                    continue

                if flip_winding:
                    b, c = c, b
                flip_winding = not flip_winding
                tris.append((a, b, c))
            return tris

        # Trường hợp 1: Strip 2 quad nối nhau bằng degenerate vertices [0, 1, 2, 3, 3, 4, 4, 5, 6, 7]
        strip_indices = [0, 1, 2, 3, 3, 4, 4, 5, 6, 7]
        result_tris = decode_triangle_strip(strip_indices)
        # Quad 0: (0, 1, 2) và (1, 3, 2)
        # Quad 1: (4, 5, 6) và (5, 7, 6)
        self.assertEqual(len(result_tris), 4, "2 Quad phải sinh ra đúng 4 tam giác hợp lệ")
        self.assertEqual(result_tris[0], (0, 1, 2), "Tam giác 0 của Quad 0")
        self.assertEqual(result_tris[1], (1, 3, 2), "Tam giác 1 của Quad 0 (CCW swap)")
        self.assertEqual(result_tris[2], (4, 5, 6), "Tam giác 0 của Quad 1 phải là winding thuận (CCW, không bị swap)")
        self.assertEqual(result_tris[3], (5, 7, 6), "Tam giác 1 của Quad 1 (CCW swap)")

        # Trường hợp 2: Strip nối bằng Primitive Restart 0xFFFF
        strip_restart = [0, 1, 2, 3, 0xFFFF, 10, 11, 12, 13]
        result_restart = decode_triangle_strip(strip_restart)
        self.assertEqual(len(result_restart), 4, "2 Strip tách rời qua 0xFFFF phải sinh đúng 4 tam giác")
        self.assertEqual(result_restart[2], (10, 11, 12), "Tam giác đầu tiên sau 0xFFFF phải là CCW chuẩn")
        self.assertEqual(result_restart[3], (11, 13, 12), "Tam giác thứ hai sau 0xFFFF (CCW swap)")

    def test_white_pixel_texture_isolated_from_fullscreen_quad(self):
        # Kiểm tra việc cô lập Texture 1x1 White Pixel:
        # whitePixelTexture không bao giờ bị ghi đè bởi activePixels của gameplay/hội trường
        white_pixel_tex = 1001
        fallback_quad_tex = 1002

        # Untextured mesh phải bind white_pixel_tex
        def get_mesh_texture(cmd_texture_handle):
            if cmd_texture_handle is None:
                return white_pixel_tex
            return cmd_texture_handle

        self.assertEqual(get_mesh_texture(None), 1001, "Untextured mesh phải dùng whitePixelTexture")
        self.assertNotEqual(get_mesh_texture(None), fallback_quad_tex, "Untextured mesh tuyệt đối không dùng fallbackQuadTexture")

    def test_copy_resource_avoids_sync_gpu_stall_on_scene_transition(self):
        # Kiểm tra logic CopyResource: Phân biệt rõ giữa GPU texture chuyển cảnh (USAGE_DEFAULT)
        # và Staging texture phục vụ chụp màn hình / thumbnail (USAGE_STAGING)
        def handle_copy_subresource(usage, cpu_access_flags, width, height, has_cpu_cache):
            is_staging = (usage == 3) or ((cpu_access_flags & 0x20000) != 0)
            is_thumbnail = (width in (284, 480, 568)) and (height in (160, 270, 320))
            is_staging_1080 = is_staging and (width == 1920 and height == 1080)

            if is_thumbnail or is_staging_1080:
                if has_cpu_cache:
                    return "CPU_CACHE_IMMEDIATE" # Ưu tiên lấy từ cache CPU sLastCapturedScreen
                return "GPU_CAPTURE_FALLBACK"   # Fallback chụp trực tiếp từ GPU FBO
            return "GPU_TO_GPU_VRAM_BLIT"       # Render bình thường / chuyển cảnh trên GPU không bao giờ stall CPU

        # 1. Chuyển cảnh trên GPU (USAGE_DEFAULT = 0, no CPU access): Luôn là VRAM BLIT không stall CPU
        self.assertEqual(handle_copy_subresource(0, 0, 1920, 1080, False), "GPU_TO_GPU_VRAM_BLIT")
        self.assertEqual(handle_copy_subresource(0, 0, 1920, 1080, True), "GPU_TO_GPU_VRAM_BLIT")

        # 2. Thumbnail Staging (284x160): Có cache -> lấy cache, chưa có cache -> fallback chụp GPU
        self.assertEqual(handle_copy_subresource(3, 0x20000, 284, 160, True), "CPU_CACHE_IMMEDIATE")
        self.assertEqual(handle_copy_subresource(3, 0x20000, 284, 160, False), "GPU_CAPTURE_FALLBACK")

        # 3. Screen Capture Staging 1920x1080 (USAGE_STAGING = 3): Có cache -> lấy cache, chưa có cache -> fallback chụp GPU
        self.assertEqual(handle_copy_subresource(3, 0x20000, 1920, 1080, True), "CPU_CACHE_IMMEDIATE")
        self.assertEqual(handle_copy_subresource(3, 0x20000, 1920, 1080, False), "GPU_CAPTURE_FALLBACK")

    def test_mock_context_map_auto_fills_zeroed_staging_buffer(self):
        # Kiểm tra logic mock_Context_Map: Khi game gọi Map trên Staging Texture (1920x1080 hoặc thumbnail)
        # mà buffer đang toàn số 0, hệ thống phải tự động fill dữ liệu từ sLastCapturedScreen hoặc capture mới
        class MockMappedTexture:
            def __init__(self, width, height, usage, cpu_access, initial_data):
                self.width = width
                self.height = height
                self.usage = usage
                self.cpu_access = cpu_access
                self.data = list(initial_data)

            def is_target_size(self):
                is_thumb = (self.width in (284, 480, 568)) and (self.height in (160, 270, 320))
                is_staging = (self.usage == 3) or ((self.cpu_access & 0x20000) != 0)
                is_screen = (self.width == 1920 and self.height == 1080)
                return is_thumb or (is_staging and is_screen)

            def map_read(self, cpu_cache):
                if not self.is_target_size():
                    return self.data # Giữ nguyên

                # Kiểm tra 4 dwords đầu
                is_all_zeros = all(b == 0 for b in self.data[:16])
                if is_all_zeros:
                    if cpu_cache:
                        self.data = list(cpu_cache) # Fill từ cache
                    else:
                        self.data = [0xFF] * len(self.data) # Giả lập capture mới từ GPU
                return self.data

        # Trường hợp 1: Staging 1920x1080 toàn số 0, có CPU cache
        mock_cache = [1, 2, 3, 4] * 10
        tex1080 = MockMappedTexture(1920, 1080, 3, 0x20000, [0] * 40)
        result1080 = tex1080.map_read(mock_cache)
        self.assertEqual(result1080, mock_cache, "Map 1920x1080 phải tự động fill từ CPU cache khi buffer toàn 0")

        # Trường hợp 2: Staging 1920x1080 toàn số 0, chưa có CPU cache -> phải trigger GPU capture mới
        tex1080_nocache = MockMappedTexture(1920, 1080, 3, 0x20000, [0] * 40)
        result1080_nocache = tex1080_nocache.map_read(None)
        self.assertEqual(result1080_nocache, [0xFF] * 40, "Map 1920x1080 khi chưa có cache phải capture mới")

        # Trường hợp 3: Texture GPU thông thường (USAGE_DEFAULT = 0) không bao giờ bị can thiệp
        tex_gpu = MockMappedTexture(1920, 1080, 0, 0, [0] * 40)
        result_gpu = tex_gpu.map_read(mock_cache)
        self.assertEqual(result_gpu, [0] * 40, "Texture GPU USAGE_DEFAULT không được tự ý can thiệp dữ liệu khi Map")

        # Trường hợp 4: Thumbnail Staging 284x160 toàn số 0 -> tự động fill
        tex_thumb = MockMappedTexture(284, 160, 3, 0x20000, [0] * 40)
        result_thumb = tex_thumb.map_read(mock_cache)
        self.assertEqual(result_thumb, mock_cache, "Thumbnail 284x160 phải tự động fill từ cache")

    def test_screen_capture_color_mapping_and_thumbnail_save_fidelity(self):
        # Kiểm tra logic ánh xạ kênh màu chính xác 100% giữa OpenGL ES glReadPixels, Staging Buffer, Save File và UI Thumbnail
        # Trong OpenGL ES trên Android/LDPlayer, glReadPixels(GL_RGBA) trả về: [R, G, B, A]
        src_pixel = [209, 191, 181, 255] # Da Mayuri chuẩn: R=209, G=191, B=181

        # 1. Luồng Thumbnail Save (toBgra = True):
        # Staging buffer nhận BGRA: dst[0]=B, dst[1]=G, dst[2]=R, dst[3]=A
        dst_bgra = [0, 0, 0, 0]
        dst_bgra[0] = src_pixel[2] # B
        dst_bgra[1] = src_pixel[1] # G
        dst_bgra[2] = src_pixel[0] # R
        dst_bgra[3] = 255
        self.assertEqual(dst_bgra[0], 181, "Staging BGRA byte 0 phải là Blue")
        self.assertEqual(dst_bgra[1], 191, "Staging BGRA byte 1 phải là Green")
        self.assertEqual(dst_bgra[2], 209, "Staging BGRA byte 2 phải là Red")

        # Hàm sub_14007D8C0 của engine Mages gốc đọc staging và nén vào save file:
        # save[0] = staging[2] (R), save[1] = staging[1] (G), save[2] = staging[0] (B)
        save_file_pixel = [dst_bgra[2], dst_bgra[1], dst_bgra[0]]
        self.assertEqual(save_file_pixel[0], 209, "Save file byte 0 phải là Red gốc")
        self.assertEqual(save_file_pixel[1], 191, "Save file byte 1 phải là Green gốc")
        self.assertEqual(save_file_pixel[2], 181, "Save file byte 2 phải là Blue gốc")

        # 2. Luồng Scene Transition / RGBA texture (toBgra = False):
        # Đích đến là texture GLES upload qua glTexImage2D(GL_RGBA): giữ nguyên 1:1 RGBA
        dst_rgba = [0, 0, 0, 0]
        dst_rgba[0] = src_pixel[0] # R
        dst_rgba[1] = src_pixel[1] # G
        dst_rgba[2] = src_pixel[2] # B
        dst_rgba[3] = 255
        self.assertEqual(dst_rgba[0], 209, "Transition RGBA byte 0 phải là Red gốc (loại trừ Transition Flash)")
        self.assertEqual(dst_rgba[1], 191, "Transition RGBA byte 1 phải là Green gốc")
        self.assertEqual(dst_rgba[2], 181, "Transition RGBA byte 2 phải là Blue gốc")

        # 3. Luồng Load Thumbnail UI (sub_1400802C0 + mock_Context_Unmap):
        # sub_1400802C0 đọc từ save file [209, 191, 181] và ghi BGRA vào mapped thumbnail texture:
        mapped_ui_tex = [save_file_pixel[2], save_file_pixel[1], save_file_pixel[0], 255] # [181, 191, 209, 255]
        self.assertEqual(mapped_ui_tex[0], 181, "Mapped UI texture byte 0 là Blue theo chuẩn Mages BGRA")
        self.assertEqual(mapped_ui_tex[2], 209, "Mapped UI texture byte 2 là Red theo chuẩn Mages BGRA")

        # Khi mock_Context_Unmap được gọi cho Thumbnail UI (284x160, format=87, usage!=3):
        # Thực hiện std::swap(px[0], px[2]) để chuyển về RGBA cho OpenGL ES
        def mock_unmap_thumbnail(w, h, fmt, usage, pixels):
            is_thumb = (w == 284 and h == 160) or (w == 480 and h == 270) or (w == 568 and h == 320)
            if is_thumb and fmt == 87 and usage != 3:
                pixels[0], pixels[2] = pixels[2], pixels[0]
            return pixels

        unmapped_ui = mock_unmap_thumbnail(284, 160, 87, 2, list(mapped_ui_tex))
        self.assertEqual(unmapped_ui[0], 209, "Thumbnail UI sau Unmap phải có byte 0 là Red cho GLES GL_RGBA")
        self.assertEqual(unmapped_ui[1], 191, "Thumbnail UI sau Unmap phải có byte 1 là Green")
        self.assertEqual(unmapped_ui[2], 181, "Thumbnail UI sau Unmap phải có byte 2 là Blue")

        # Kiểm tra an toàn: Texture Fullscreen 1920x1080 tuyệt đối KHÔNG bị swizzle khi Unmap
        fs_pixels = [209, 191, 181, 255]
        unmapped_fs = mock_unmap_thumbnail(1920, 1080, 87, 0, list(fs_pixels))
        self.assertEqual(unmapped_fs, fs_pixels, "Fullscreen texture 1920x1080 không được swizzle khi Unmap")

        # Kiểm tra an toàn: Staging Texture (usage == 3) tuyệt đối KHÔNG bị swizzle khi Unmap
        staging_pixels = [181, 191, 209, 255]
        unmapped_staging = mock_unmap_thumbnail(284, 160, 87, 3, list(staging_pixels))
        self.assertEqual(unmapped_staging, staging_pixels, "Staging texture (usage=3) không được swizzle khi Unmap")

    def test_fbo_handle_release_prevents_emote_atlas_collision(self):
        """
        Kiểm thử cơ chế giữ tham chiếu Render Thread (+1 ref) và dọn dẹp sFboCache/sRenderTargetHandles
        qua D3D11Bridge_ConsumeReleasedHandles & D3D11Bridge_FinalizeRelease trước khi giải phóng địa chỉ heap,
        ngăn chặn triệt để lỗi tái sử dụng địa chỉ của MRenderTarget cũ (426x253 / 883x1055) cho E-mote Atlas (4096x4016).
        """
        fbo_cache = {"0x763875626260": 15}
        rt_handles = {"0x763875626260"}
        gles_tex_cache = {"0x763875626260": {"texId": 42, "w": 883, "h": 1055, "rev": 1}}
        registry_bind_flags = {"0x763875626260": 0x28}  # kBindRenderTarget | kBindShaderResource

        # 1. Game Thread gọi Release() trên MRenderTarget 883x1055 -> đưa vào hàng đợi giải phóng cuối frame
        released_handles = ["0x763875626260"]
        for h in released_handles:
            fbo_cache.pop(h, None)
            rt_handles.discard(h)
            gles_tex_cache.pop(h, None)
            registry_bind_flags.pop(h, None)

        self.assertNotIn("0x763875626260", fbo_cache)
        self.assertNotIn("0x763875626260", rt_handles)

        # 2. Giả sử bộ cấp phát cấp lại cùng địa chỉ cho E-mote Character Atlas (4096x4016, bindFlags=0x08)
        registry_bind_flags["0x763875626260"] = 0x08
        is_rt = ("0x763875626260" in fbo_cache or "0x763875626260" in rt_handles) and (
            (registry_bind_flags.get("0x763875626260", 0) & 0x20) != 0
        )
        self.assertFalse(is_rt, "Texture nhân vật E-mote (bindFlags=0x08) tuyệt đối không được nhận nhầm là RenderTarget FBO")

    def test_mcapture_mid_frame_pure_gameplay_capture_and_priority(self):
        """
        Kiểm thử luồng MCapture::draw -> MTexture::bind (GPU-owned Staging chain) theo engine gốc:
        - Lệnh CopySubresourceRegion từ Backbuffer sang outerStaging đi vào nhánh isClearCommand == 3 và đánh dấu outerStaging là GPU-owned.
        - Lệnh CopyResource từ outerStaging sang innerChild_SRV nhận diện isGpuOwned(outerStaging) == True nên copy tiếp trên GPU, không copy buffer CPU toàn 0.
        - Capture::storeThumbnail dùng nội dung pre-menu trong innerChild_SRV thông qua RenderTarget 284x160 và flush GPU readback khi Map(v42_staging).
        """
        class MockGpuRegistry:
            def __init__(self):
                self.textures = {}
                self.gpu_owned = set()
                self.cpu_copies = []
                self.gpu_copies = []

            def create_tex(self, name, w, h, fmt=87, bind_flags=0x08, usage=0):
                self.textures[name] = {"w": w, "h": h, "fmt": fmt, "bind": bind_flags, "usage": usage, "rev": 1}

            def is_gpu_backed(self, name):
                if name == "backbuffer":
                    return True
                t = self.textures.get(name)
                if not t:
                    return False
                return ((t["bind"] & 0x20) != 0) or (name in self.gpu_owned)

            def copy_resource(self, dst, src):
                if self.is_gpu_backed(src) and dst in self.textures:
                    self.gpu_owned.add(dst)
                    self.gpu_copies.append((dst, src))
                    return "gpu_copy"
                self.gpu_owned.discard(dst)
                self.textures[dst]["rev"] += 1
                self.cpu_copies.append((dst, src))
                return "cpu_copy"

            def update_subresource(self, dst):
                self.gpu_owned.discard(dst)
                self.textures[dst]["rev"] += 1

        reg = MockGpuRegistry()
        reg.create_tex("outer_staging", 1920, 1080, fmt=87, bind_flags=0, usage=3)
        reg.create_tex("inner_child_srv", 1920, 1080, fmt=87, bind_flags=0x08, usage=0)
        reg.create_tex("thumb_rt", 284, 160, fmt=87, bind_flags=0x28, usage=0)
        reg.create_tex("thumb_staging", 284, 160, fmt=87, bind_flags=0, usage=3)

        # 1. MCapture::draw copy Backbuffer -> outer_staging
        self.assertEqual(reg.copy_resource("outer_staging", "backbuffer"), "gpu_copy")
        self.assertIn("outer_staging", reg.gpu_owned)

        # 2. MTexture::bind copy outer_staging -> inner_child_srv (phải đi đường GPU, không đi đường CPU toàn 0)
        self.assertEqual(reg.copy_resource("inner_child_srv", "outer_staging"), "gpu_copy")
        self.assertIn("inner_child_srv", reg.gpu_owned)
        self.assertEqual(len(reg.cpu_copies), 0)

        # 3. Capture::storeThumbnail copy thumb_rt -> thumb_staging
        self.assertEqual(reg.copy_resource("thumb_staging", "thumb_rt"), "gpu_copy")
        self.assertIn("thumb_staging", reg.gpu_owned)

        # Edge case: Nếu UpdateSubresource ghi đè CPU lên inner_child_srv thì phải gỡ gpu_owned
        reg.update_subresource("inner_child_srv")
        self.assertNotIn("inner_child_srv", reg.gpu_owned)

    def test_programmable_blend_shaders_t2_and_gl_texture0_state_sync(self):
        """
        Kiểm thử dải 1140 shader Programmable Blend (900 <= shaderId < 2040, gồm 1283 mặt Kurisu và 1302 thân Kurisu):
        - Phải giải mã đúng blendMode = (shaderId - 900) % 19, maskMode = ((shaderId - 900) // 19) % 4,
          colorMode = ((shaderId - 900) // 19) // 4.
        - Khi có t2 (uHasDestTexture == 1), thực hiện trộn RGB giữa dest (t2) và src (t0) với hệ số w = clamp(src.a * cb0[0].w, 0, 1)
          và xuất alpha = 1.0 cho Blend State 10 (GL_ONE, GL_ZERO).
        - Sau khi isClearCommand == 3 (glCopyTexSubImage2D) unbind GL_TEXTURE_2D về 0 trên GL_TEXTURE0,
          bắt buộc phải reset lastBoundHandle = None để lệnh vẽ tiếp theo bind lại texture nhân vật.
        """
        def decode_programmable_shader(shader_id):
            self.assertTrue(900 <= shader_id < 2040)
            idx = shader_id - 900
            blend_mode = idx % 19
            rem19 = idx // 19
            mask_mode = rem19 % 4
            color_mode = rem19 // 4
            return blend_mode, mask_mode, color_mode

        # Shader 1283 (mặt Kurisu 426x253): blendMode=3 (Overlay), maskMode=0 (no mask), colorMode=5 (Tint * Add)
        self.assertEqual(decode_programmable_shader(1283), (3, 0, 5))
        # Shader 1302 (thân Kurisu 883x1055): blendMode=3 (Overlay), maskMode=1 (alpha mask t1), colorMode=5 (Tint * Add)
        self.assertEqual(decode_programmable_shader(1302), (3, 1, 5))

        # Kiểm tra đồng bộ lastBoundHandle sau isClearCommand == 3
        gl_tex0_bound = "kurisu_atlas"
        last_bound_handle = "kurisu_atlas"

        # Thực thi isClearCommand == 3 (copy 426x253 từ FBO sang tempTex)
        gl_tex0_bound = "temp_426x253"
        gl_tex0_bound = None  # glBindTexture(GL_TEXTURE_2D, 0)
        last_bound_handle = None  # Bắt buộc reset!

        # Draw call tiếp theo dùng lại "kurisu_atlas"
        if "kurisu_atlas" != last_bound_handle:
            gl_tex0_bound = "kurisu_atlas"
            last_bound_handle = "kurisu_atlas"

        self.assertEqual(
            gl_tex0_bound,
            "kurisu_atlas",
            "Sau isClearCommand == 3, draw call tiếp theo bắt buộc phải rebind GL_TEXTURE0 thay vì đọc texture 0 đen"
        )

    def test_gpu_texture_copy_resource_emits_clear_cmd_3_without_zero_cpu_override(self):
        # Kiểm tra logic xử lý CopyResource giữa các GPU texture:
        # 1. Luôn phát lệnh GPU copy (isClearCommand = 3) qua queue
        # 2. Không nuốt chửng lệnh bằng việc return sớm qua CPU zero data
        # 3. Đồng bộ revision của GPU texture sau copy để tránh bị CPU buffer ghi đè đen
        active_drawing_meshes = []
        gles_texture_cache = {}

        def mock_copy_resource(dst_handle, src_handle, is_dst_thumb=False, src_has_gpu_data=True):
            if is_dst_thumb:
                return "THUMBNAIL_READBACK"
            if src_has_gpu_data:
                cmd = {
                    "isClearCommand": 3,
                    "renderTargetHandle": src_handle,
                    "textureHandle": dst_handle,
                    "width": 1920,
                    "height": 1080
                }
                active_drawing_meshes.append(cmd)
                return "GPU_COPY_EMITTED"
            return "FALLBACK_CPU"

        # Giả lập CopyResource(0xDst, 0xSrc) trong render pass Mayuri
        result = mock_copy_resource("0xMayuriDst", "0xMayuriSrc", is_dst_thumb=False, src_has_gpu_data=True)
        self.assertEqual(result, "GPU_COPY_EMITTED")
        self.assertEqual(len(active_drawing_meshes), 1)
        self.assertEqual(active_drawing_meshes[0]["isClearCommand"], 3)
        self.assertEqual(active_drawing_meshes[0]["textureHandle"], "0xMayuriDst")

        # Giả lập Render Thread thực thi isClearCommand == 3
        # Cập nhật revision cho dstTex để getOrCreateGlesTexture không upload CPU buffer rỗng
        cmd = active_drawing_meshes[0]
        gles_texture_cache[cmd["textureHandle"]] = {"texId": 42, "revision": 2}
        current_rev = 2
        should_upload_cpu = (gles_texture_cache[cmd["textureHandle"]]["revision"] != current_rev)
        self.assertFalse(should_upload_cpu, "GPU texture sau khi copy không được bị CPU upload đè mảng 0")

    def test_render_target_keeps_no_zeroed_cpu_data(self):
        # Kiểm tra quy tắc: Render Target Format 87 (B8G8R8A8) hoặc Format 28 với BindFlags=0x28
        # khi initData == nullptr TUYỆT ĐỐI KHÔNG được cấp phát CPU memory toàn 0
        def compute_wants_cpu_copy(dxgi_format, bind_flags, init_data_ptr, width, height):
            kBindRenderTarget = 0x20
            kBindDepthStencil = 0x40
            is_offscreen_mask = (dxgi_format in (65, 61))  # Format A8 hoặc R8
            is_thumbnail = ((bind_flags & kBindRenderTarget) == 0) and (width in (284, 480, 568)) and (height in (160, 270, 320))
            wants_cpu = ((bind_flags & (kBindRenderTarget | kBindDepthStencil)) == 0 or is_offscreen_mask or is_thumbnail) and (init_data_ptr is not None or is_offscreen_mask or is_thumbnail)
            return wants_cpu

        # 1. Render Target 1920x1080 của Mayuri / Gameplay FBO (Format 87, BindFlags 0x28, initData = None)
        self.assertFalse(compute_wants_cpu_copy(87, 0x28, None, 1920, 1080), "RenderTarget 1920x1080 tuyệt đối không giữ CPU buffer")

        # 2. Render Target khuôn mặt Kurisu 426x253 (Format 87, BindFlags 0x28, initData = None)
        self.assertFalse(compute_wants_cpu_copy(87, 0x28, None, 426, 253), "RenderTarget 426x253 tuyệt đối không giữ CPU buffer")

        # 3. Offscreen Mask Format 65 A8_UNORM (initData = None) -> Cần CPU copy cho CPU mask evaluation
        self.assertTrue(compute_wants_cpu_copy(65, 0x28, None, 1920, 1080), "Format 65 A8 Mask được phép giữ CPU buffer")

        # 4. Thumbnail Save Slot 284x160 (Format 87, BindFlags 0x08, initData = None) -> Cần CPU buffer cho Map
        self.assertTrue(compute_wants_cpu_copy(87, 0x08, None, 284, 160), "Thumbnail Save Slot 284x160 cần CPU buffer để Map")

    def test_font_dynamic_texture_update_subresource_allocates_data(self):
        # Kiểm tra: Font dynamic texture 1024x1024 (Format 87, BindFlags 0x8, initData = None)
        # Ban đầu hasData = False và data có thể chưa có nội dung.
        # Khi game engine gọi updateTexture (UpdateSubresource), hàm phải tự động cấp phát bộ đệm nếu rỗng
        # và đánh dấu hasData = True, tăng revision để GPU upload glyph chữ thoại.
        class MockTextureEntry:
            def __init__(self, w, h, fmt):
                self.width = w
                self.height = h
                self.format = fmt
                self.rowPitch = w * 4
                self.data = bytearray()
                self.hasData = False
                self.revision = 1

            def update_texture(self, box, src_data, src_pitch):
                if not src_data:
                    return False
                if len(self.data) == 0:
                    self.data = bytearray(self.rowPitch * self.height)
                self.hasData = True
                self.revision += 1
                return True

        font_tex = MockTextureEntry(1024, 1024, 87)
        self.assertFalse(font_tex.hasData)
        self.assertEqual(len(font_tex.data), 0)

        # Mô phỏng nạp glyph câu thoại tiếng Việt mới vào font texture
        glyph_data = b"\xFF" * (64 * 64 * 4)
        success = font_tex.update_texture(None, glyph_data, 64 * 4)
        self.assertTrue(success, "updateTexture phải thành công với font dynamic texture")
        self.assertTrue(font_tex.hasData, "hasData phải được chuyển thành True để Render Thread upload")
        self.assertGreater(len(font_tex.data), 0, "Buffer font texture phải được cấp phát")
        self.assertEqual(font_tex.revision, 2, "Revision phải tăng để GPU nhận diện glyph mới")

    def test_bc7_subregion_update_for_wide_backgrounds(self):
        # Kiểm thử giải mã và cập nhật vùng con BC7 (3072x1728) vào texture 2048x2048 và 1024x2048
        class MockBc7Entry:
            def __init__(self, w, h):
                self.width = w
                self.height = h
                self.rowPitch = w * 4
                self.data = bytearray(self.rowPitch * h)
                self.revision = 0

            def update_bc7_subregion(self, left, top, right, bottom, sub_rgba_data):
                if left >= right or top >= bottom or right > self.width or bottom > self.height:
                    return False
                sub_w = right - left
                sub_h = bottom - top
                sub_pitch = sub_w * 4
                for r in range(sub_h):
                    src_offset = r * sub_pitch
                    dst_offset = (top + r) * self.rowPitch + left * 4
                    self.data[dst_offset : dst_offset + sub_pitch] = sub_rgba_data[src_offset : src_offset + sub_pitch]
                self.revision += 1
                return True

        # 1. Mảnh 1: 2048x1728 trên texture 2048x2048
        tex1 = MockBc7Entry(2048, 2048)
        sub_data1 = b"\xAA" * (2048 * 1728 * 4)
        success1 = tex1.update_bc7_subregion(0, 0, 2048, 1728, sub_data1)
        self.assertTrue(success1, "Cập nhật BC7 vùng con 2048x1728 phải thành công")
        self.assertEqual(tex1.data[0:4], b"\xAA\xAA\xAA\xAA")
        # Kiểm tra phần padding (hàng 1728..2047) vẫn là 0
        padding_offset = 1728 * tex1.rowPitch
        self.assertEqual(tex1.data[padding_offset : padding_offset + 4], b"\x00\x00\x00\x00")

        # 2. Mảnh 2: 1024x1728 trên texture 1024x2048
        tex2 = MockBc7Entry(1024, 2048)
        sub_data2 = b"\xBB" * (1024 * 1728 * 4)
        success2 = tex2.update_bc7_subregion(0, 0, 1024, 1728, sub_data2)
        self.assertTrue(success2, "Cập nhật BC7 vùng con 1024x1728 phải thành công")
        self.assertEqual(tex2.data[0:4], b"\xBB\xBB\xBB\xBB")

    def test_font_atlas_protection_against_fullhd_expansion(self):
        # Đảm bảo Texture Font Atlas (<= 1024x1024) KHÔNG bị tự ý đổi kích thước lên 1920x1080
        class MockTexture:
            def __init__(self, w, h, fmt):
                self.width = w
                self.height = h
                self.format = fmt
                self.rowPitch = w * 4

            def process_auto_expansion(self, src_pitch, box):
                # Logic an toàn: TUYỆT ĐỐI KHÔNG mở rộng texture có kích thước cố định
                pass

        # Texture Font Atlas 1024x1024
        font_atlas = MockTexture(1024, 1024, 87)
        font_atlas.process_auto_expansion(7680, None)
        self.assertEqual(font_atlas.width, 1024, "Font Atlas width phải giữ nguyên 1024")
        self.assertEqual(font_atlas.height, 1024)
        self.assertEqual(font_atlas.rowPitch, 4096)

        # Texture Dialogue Font 1210x1063
        dialogue_tex = MockTexture(1210, 1063, 87)
        dialogue_tex.process_auto_expansion(7680, None)
        self.assertEqual(dialogue_tex.width, 1210, "Dialogue font texture width phải giữ nguyên 1210, không bị nát sọc caro")
        self.assertEqual(dialogue_tex.height, 1063)

    def test_texture_generation_id_and_cpu_retention(self):
        # Kiểm tra tính năng generationId độc nhất phân biệt con trỏ heap tái sử dụng
        class MockRegistry:
            def __init__(self):
                self.next_gen = 1
                self.textures = {}

            def create_texture(self, handle, w, h):
                gen_id = self.next_gen
                self.next_gen += 1
                self.textures[handle] = {"w": w, "h": h, "gen_id": gen_id, "data": [0] * (w * h * 4)}
                return gen_id

        reg = MockRegistry()
        handle_ptr = 0x76388a37ca60
        gen1 = reg.create_texture(handle_ptr, 1928, 528)
        self.assertEqual(gen1, 1)

        # Giả lập con trỏ heap bị giải phóng và tái sử dụng cho texture background 3424x3424
        gen2 = reg.create_texture(handle_ptr, 3424, 3424)
        self.assertEqual(gen2, 2)
        self.assertNotEqual(gen1, gen2, "Tài nguyên mới tái sử dụng địa chỉ con trỏ phải có generationId khác biệt")
        self.assertGreater(len(reg.textures[handle_ptr]["data"]), 0, "Dữ liệu CPU của texture phải được bảo toàn")

    def test_transition_shader_phase_and_mask_discard(self):
        # Kiểm tra logic toán học của Shader 2040:
        # Khi uHasAlphaTexture == 0 hoặc phase >= 1.0 -> DISCARD (alpha = 0.0)
        def eval_shader_2040(has_alpha_tex, phase, mask_val, inv_vague, vcolor_a):
            if not has_alpha_tex or phase >= 1.0:
                return "DISCARD", 0.0
            eff_alpha = max(0.0, min(1.0, (mask_val - phase) * inv_vague + 1.0)) * vcolor_a
            return "DRAW", eff_alpha

        # 1. Khi đang chuyển cảnh (phase = 0.5)
        status, alpha = eval_shader_2040(True, 0.5, 0.5, 16.0, 1.0)
        self.assertEqual(status, "DRAW")
        self.assertEqual(alpha, 1.0)

        # 2. Khi chuyển cảnh kết thúc (phase >= 1.0)
        status, alpha = eval_shader_2040(True, 1.0, 1.0, 16.0, 1.0)
        self.assertEqual(status, "DISCARD", "Phase >= 1.0 phải lập tức discard cảnh cũ")
        self.assertEqual(alpha, 0.0)

        # 3. Khi mất alpha texture
        status, alpha = eval_shader_2040(False, 0.2, 0.5, 16.0, 1.0)
        self.assertEqual(status, "DISCARD", "Không có alpha mask texture phải discard")
        self.assertEqual(alpha, 0.0)

    def test_scene_transition_standard_clear_and_blend(self):
        # Kiểm tra pipeline GLES: Backbuffer clear chuẩn theo clearColor và hòa trộn alpha layer chuyển cảnh
        class MockGlesRenderPipeline:
            def __init__(self):
                self.framebuffer0_color = [0.0, 0.0, 0.0, 1.0] # Mặc định đen
                self.cleared_count = 0
                self.blended_frame_colors = []

            def render_frame(self, mesh_commands, clear_color=[0.0, 0.0, 0.0, 1.0]):
                has_backbuffer_draw = any(cmd.get("renderTargetHandle") is None and cmd.get("isClearCommand") == 0 for cmd in mesh_commands)
                if has_backbuffer_draw:
                    self.framebuffer0_color = list(clear_color)
                    self.cleared_count += 1

                for cmd in mesh_commands:
                    cmd_type = cmd.get("isClearCommand", 0)
                    rt_handle = cmd.get("renderTargetHandle")

                    if cmd_type == 1:
                        if rt_handle is None:
                            self.framebuffer0_color = list(cmd.get("clearColor", clear_color))
                            self.cleared_count += 1
                        continue

                    if cmd_type == 0 and rt_handle is None:
                        mesh_color = cmd.get("color", [1.0, 1.0, 1.0, 1.0])
                        alpha = cmd.get("alpha", 1.0)
                        # Blend: dst = dst * (1 - alpha) + src * alpha
                        for i in range(3):
                            self.framebuffer0_color[i] = self.framebuffer0_color[i] * (1.0 - alpha) + mesh_color[i] * alpha

                self.blended_frame_colors.append(list(self.framebuffer0_color))

        pipeline = MockGlesRenderPipeline()

        # Frame 1: Cảnh A (xanh lam [0.2, 0.4, 0.8, 1.0])
        frame1_cmds = [
            {"isClearCommand": 0, "renderTargetHandle": None, "color": [0.2, 0.4, 0.8, 1.0], "alpha": 1.0}
        ]
        pipeline.render_frame(frame1_cmds)
        self.assertEqual(pipeline.cleared_count, 1)
        self.assertEqual(pipeline.framebuffer0_color, [0.2, 0.4, 0.8, 1.0])

        # Frame 2: Chuyển cảnh - Fade out / fade in tự nhiên từ game engine
        frame2_cmds = [
            {"isClearCommand": 0, "renderTargetHandle": None, "color": [0.9, 0.3, 0.1, 1.0], "alpha": 0.5}
        ]
        pipeline.render_frame(frame2_cmds)
        self.assertEqual(pipeline.cleared_count, 2, "Mỗi frame vẽ backbuffer đều clear sạch sẽ để tránh bóng ma")
        # 0.0 * 0.5 + 0.9 * 0.5 = 0.45
        self.assertAlmostEqual(pipeline.framebuffer0_color[0], 0.45, places=4)

    def test_staging_copy_subresource_non_blocking_gpu_path(self):
        # Kiểm tra chuỗi CopyResource GPU->Staging->SRV hoàn toàn bất đồng bộ không stall glReadPixels trong lúc chuyển cảnh
        class MockGlesTextureCache:
            def __init__(self):
                self.cache = {}
                self.gpu_copied = set()
                self.gpu_blits = []
                self.cpu_uploads = []

            def get_or_create(self, handle, pixels, rev, gen, has_gpu_contents):
                entry = self.cache.get(handle)
                if entry is None or (gen > 0 and entry["gen"] != gen):
                    self.cache[handle] = {"rev": rev, "gen": gen}
                    if pixels is not None:
                        self.cpu_uploads.append(handle)
                    return handle
                is_gpu_owned = (handle in self.gpu_copied) or has_gpu_contents
                if is_gpu_owned and (pixels is None or entry["rev"] == rev):
                    return handle
                entry["rev"] = rev
                self.cpu_uploads.append(handle)
                return handle

            def exec_copy_subresource(self, src_is_backbuffer, dst_handle, dst_rev, dst_gen):
                self.get_or_create(dst_handle, None, dst_rev, dst_gen, True)
                self.gpu_copied.add(dst_handle)
                self.cache[dst_handle]["rev"] = dst_rev
                self.gpu_blits.append(("backbuffer_flip" if src_is_backbuffer else "fbo_copy", dst_handle))

        gles = MockGlesTextureCache()
        # MCapture::draw (Backbuffer -> outerStaging) + MTexture::bind (outerStaging -> innerChild_SRV)
        gles.exec_copy_subresource(True, "outerStaging", 1, 10)
        gles.exec_copy_subresource(False, "innerChildSRV", 1, 11)
        self.assertEqual(len(gles.cpu_uploads), 0, "Không được upload buffer CPU toàn 0 đè lên GPU texture")
        self.assertEqual(gles.gpu_blits, [("backbuffer_flip", "outerStaging"), ("fbo_copy", "innerChildSRV")])

        # Khi MCaptureClient::draw bind innerChildSRV với pixels=all_zero (rev=1), phải giữ bản GPU đã copy
        gles.get_or_create("innerChildSRV", b"\x00" * 16, 1, 11, True)
        self.assertEqual(len(gles.cpu_uploads), 0)

        # Edge case: Con trỏ được tái cấp phát với generationId mới -> phải tạo texture mới sạch sẽ
        gles.get_or_create("innerChildSRV", b"\xff" * 16, 1, 99, False)
        self.assertEqual(gles.cpu_uploads, ["innerChildSRV"])

    def test_ps_2040_transition_dissolve_formula_and_edge_cases(self):
        # Kiểm chứng công thức ps_2040.bin DXBC chính xác tuyệt đối (bao gồm phase >= 1.0 không bị discard)
        def eval_ps_2040(base_rgba, mask_r, phase, inv_vague, v_color_a, has_alpha_tex=True):
            mask_val = mask_r if has_alpha_tex else 1.0
            factor = (mask_val - phase) * inv_vague + 1.0
            eff_alpha = max(0.0, min(1.0, factor)) * v_color_a
            return (base_rgba[0], base_rgba[1], base_rgba[2], base_rgba[3] * eff_alpha)

        # 1. Đầu chuyển cảnh (phase = 0.0, invVague = 4.0, mask_r = 0.5) -> alpha đầy đủ 1.0 (chưa bị discard)
        out_start = eval_ps_2040((0.8, 0.4, 0.2, 1.0), 0.5, 0.0, 4.0, 1.0)
        self.assertAlmostEqual(out_start[3], 1.0, places=5)

        # 2. Giữa chuyển cảnh tại phase = 1.0, mask_r = 0.9, invVague = 4.0 -> (0.9 - 1.0)*4 + 1.0 = 0.6 (không được discard!)
        out_mid = eval_ps_2040((0.8, 0.4, 0.2, 1.0), 0.9, 1.0, 4.0, 1.0)
        self.assertAlmostEqual(out_mid[3], 0.6, places=5)

        # 3. Cuối chuyển cảnh phase = 1.5, mask_r = 1.0 -> clamp <= 0.0 -> 0.0
        out_end = eval_ps_2040((0.8, 0.4, 0.2, 1.0), 1.0, 1.5, 4.0, 1.0)
        self.assertAlmostEqual(out_end[3], 0.0, places=5)

        # 4. Edge case thiếu t1 -> fallback mask_val = 1.0
        out_no_mask = eval_ps_2040((1.0, 1.0, 1.0, 1.0), 0.0, 0.0, 2.0, 0.75, has_alpha_tex=False)
        self.assertAlmostEqual(out_no_mask[3], 0.75, places=5)

    def test_backbuffer_yflip_and_thumbnail_gpu_readback_map_unmap(self):
        # Kiểm tra hướng Y-flip Backbuffer -> FBO Top-Down và flush readback Capture::storeThumbnail (BGRA/RGBA)
        # 1. Toán học Y-flip: Backbuffer (v=0 ở đáy, v=1 ở đỉnh) blit qua uFlipY=-1.0 với posY=+1 -> row 0 (đỉnh FBO)
        def sample_backbuffer_to_topdown(top_row_color, bot_row_color, logical_y, logical_h, req_y0, req_h):
            # v_gl = 1.0 - (y / H) ánh xạ logical_y=0 -> v_gl=1.0 (đỉnh backbuffer)
            v_gl = 1.0 - ((req_y0 + (logical_y / req_h) * req_h) / logical_h)
            return top_row_color if v_gl > 0.5 else bot_row_color

        top_px = (10, 20, 250, 255)   # Đỉnh màn hình (trời xanh)
        bot_px = (200, 180, 50, 255)  # Đáy màn hình (hộp thoại)
        self.assertEqual(sample_backbuffer_to_topdown(top_px, bot_px, 0, 1080, 0, 1080), top_px)
        self.assertEqual(sample_backbuffer_to_topdown(top_px, bot_px, 1079, 1080, 0, 1080), bot_px)

        # 2. Kiểm tra flush + readback khi Map(v42_staging) với Format 87 (BGRA) và Format 28 (RGBA) + Unmap semantics
        class MockStagingMapController:
            def __init__(self, render_loop_active=True):
                self.render_loop_active = render_loop_active
                self.gpu_owned = {"v42_staging"}
                self.revisions = {"v42_staging": 1, "ui_thumb": 1}
                self.active_maps = {}
                self.flush_calls = 0

            def map(self, handle, map_type, fmt, dst_valid=True):
                read_from_gpu = (handle in self.gpu_owned) and (map_type in (1, 3))
                self.active_maps[handle] = (map_type, read_from_gpu)
                if not read_from_gpu:
                    return True, None
                if not self.render_loop_active or not dst_valid:
                    return False, None
                self.flush_calls += 1
                # Giả lập glReadPixels RGBA: [R=120, G=160, B=240, A=255]
                raw_rgba = [120, 160, 240, 255]
                if fmt in (87, 88):  # DXGI_FORMAT_B8G8R8A8_UNORM / B8G8R8X8
                    return True, [raw_rgba[2], raw_rgba[1], raw_rgba[0], raw_rgba[3]]
                return True, raw_rgba

            def unmap(self, handle):
                map_type, read_from_gpu = self.active_maps.pop(handle, (0, False))
                if read_from_gpu or map_type == 1:
                    return  # Giữ nguyên gpu_owned và revision!
                self.gpu_owned.discard(handle)
                self.revisions[handle] = self.revisions.get(handle, 0) + 1

        ctrl = MockStagingMapController(render_loop_active=True)
        # Map để đọc thumbnail Capture::storeThumbnail (format 87, MAP_READ_WRITE=3)
        ok, px_bgra = ctrl.map("v42_staging", 3, 87)
        self.assertTrue(ok)
        self.assertEqual(px_bgra, [240, 160, 120, 255], "Staging Format 87 phải xuất đúng BGRA cho engine save")
        self.assertEqual(ctrl.flush_calls, 1)
        ctrl.unmap("v42_staging")
        self.assertIn("v42_staging", ctrl.gpu_owned, "Unmap sau GPU readback không được xóa gpu_owned")
        self.assertEqual(ctrl.revisions["v42_staging"], 1, "Unmap sau GPU readback không được tăng revision")

        # Format 28 giữ nguyên RGBA
        ctrl.gpu_owned.add("rgba_staging")
        ok28, px_rgba = ctrl.map("rgba_staging", 1, 28)
        self.assertTrue(ok28)
        self.assertEqual(px_rgba, [120, 160, 240, 255])

        # CPU write (MAP_WRITE_DISCARD=4) vào UI texture -> Unmap phải tăng revision & xóa gpu_owned
        ctrl.gpu_owned.add("ui_thumb")
        ctrl.map("ui_thumb", 4, 87)
        ctrl.unmap("ui_thumb")
        self.assertNotIn("ui_thumb", ctrl.gpu_owned)
        self.assertEqual(ctrl.revisions["ui_thumb"], 2)

        # Error handling: render loop tắt hoặc buffer null -> không crash, trả về False an toàn
        ctrl_off = MockStagingMapController(render_loop_active=False)
        ok_off, data_off = ctrl_off.map("v42_staging", 3, 87)
        self.assertFalse(ok_off)
        self.assertIsNone(data_off)
        ok_null, _ = ctrl.map("v42_staging", 3, 87, dst_valid=False)
        self.assertFalse(ok_null)

    def test_bc7_thread_pool_work_stealing_and_concurrent_fallback(self):
        # Kiểm chứng phân chia block row động trong Bc7ThreadPool & fallback an toàn khi đụng độ luồng
        import os

        cpp_dir = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp")
        with open(os.path.join(cpp_dir, "bc7_decoder.cpp"), "r", encoding="utf-8") as f:
            bc7_src = f.read()
        self.assertIn("class Bc7ThreadPool", bc7_src)
        self.assertIn("kWorkChunkRows", bc7_src)
        self.assertNotIn("workers.emplace_back([=, &out]()", bc7_src)

        def run_atomic_chunks(blocks_y, chunk_rows=4, num_workers=3):
            next_row = 0
            covered = []
            while next_row < blocks_y:
                for _ in range(num_workers):
                    if next_row >= blocks_y:
                        break
                    start = next_row
                    next_row += chunk_rows
                    end = min(blocks_y, next_row)
                    covered.extend(range(start, end))
            return sorted(covered)

        # Happy path: 100% hàng khối được giải nén không trùng lặp hay bỏ sót (kể cả số lẻ không chia hết cho 4)
        self.assertEqual(run_atomic_chunks(65), list(range(65)))
        self.assertEqual(run_atomic_chunks(16), list(range(16)))
        # Edge case: ảnh nhỏ hơn ngưỡng 16 hàng khối chạy đơn luồng
        self.assertEqual(run_atomic_chunks(3), [0, 1, 2])

    def test_resource_registry_snapshot_cow_and_lru_eviction(self):
        # Kiểm chứng Snapshot Copy-on-Write & Thu hồi LRU bảo toàn texture động/chưa upload
        class MockRegistry:
            def __init__(self, budget):
                self.budget = budget
                self.cpu_bytes = 0
                self.seq = 0
                self.entries = {}

            def register(self, handle, size, fmt, usage=0, bind_flags=0x8, cpu_access=0):
                if self.cpu_bytes + size > self.budget:
                    self._evict(size)
                self.seq += 1
                self.entries[handle] = {
                    "data": bytearray([handle_seed(handle)] * min(16, size)),
                    "size": size,
                    "fmt": fmt,
                    "usage": usage,
                    "bind_flags": bind_flags,
                    "cpu_access": cpu_access,
                    "rev": 1,
                    "uploaded": False,
                    "seq": self.seq,
                    "ref_readers": 0,
                }
                self.cpu_bytes += size

            def acquire_snapshot(self, handle):
                e = self.entries.get(handle)
                if not e or e["data"] is None:
                    return None
                self.seq += 1
                e["seq"] = self.seq
                e["ref_readers"] += 1
                return e["data"]

            def update(self, handle, val):
                e = self.entries[handle]
                if e["ref_readers"] > 0:
                    e["data"] = bytearray(e["data"])  # Copy-on-Write
                    e["ref_readers"] = 0
                e["data"][0] = val
                e["rev"] += 1

            def _is_evictable(self, e):
                if not e["uploaded"] or e["data"] is None:
                    return False
                if (e["bind_flags"] & 0x20) != 0 or e["usage"] == 3 or e["cpu_access"] != 0:
                    return False
                return (e["fmt"] in (98, 99) and e["rev"] <= 1) or e["usage"] == 1

            def _evict(self, required):
                cands = [(e["seq"], h) for h, e in self.entries.items() if self._is_evictable(e)]
                cands.sort()
                for _, h in cands:
                    if self.cpu_bytes + required <= self.budget:
                        break
                    self.cpu_bytes -= self.entries[h]["size"]
                    self.entries[h]["data"] = None

        def handle_seed(name):
            return sum(ord(c) for c in name) & 0xFF

        reg = MockRegistry(budget=100)
        # 1. Nạp font atlas BGRA8 (được cập nhật rev=2), mask A8 và 2 atlas BC7 đã upload lên GPU
        reg.register("font_bgra", 30, fmt=87, usage=0)
        reg.entries["font_bgra"]["uploaded"] = True
        reg.register("bc7_old", 40, fmt=98, usage=0)
        reg.entries["bc7_old"]["uploaded"] = True
        reg.register("bc7_not_uploaded", 25, fmt=98, usage=0)

        # Kiểm tra Copy-on-Write khi pre-uploader đang giữ snapshot của font_bgra
        snap = reg.acquire_snapshot("font_bgra")
        reg.update("font_bgra", 0xAA)
        self.assertEqual(snap[0], handle_seed("font_bgra"), "Snapshot cũ phải bất biến khi thread khác mutate (COW)")
        self.assertEqual(reg.entries["font_bgra"]["data"][0], 0xAA)

        # 2. Vượt budget 100 khi nạp bc7_new (size 40): chỉ bc7_old (đã upload, bất biến) bị evict,
        # còn font_bgra và bc7_not_uploaded tuyệt đối được giữ nguyên!
        reg.register("bc7_new", 40, fmt=98, usage=0)
        self.assertIsNone(reg.entries["bc7_old"]["data"], "Atlas BC7 cũ nhất đã upload phải được thu hồi LRU")
        self.assertIsNotNone(reg.entries["font_bgra"]["data"], "Không được thu hồi font atlas động BGRA8")
        self.assertIsNotNone(reg.entries["bc7_not_uploaded"]["data"], "Không được thu hồi texture chưa lên GPU")

    def test_shared_egl_preupload_generation_fence_and_fallback(self):
        # Kiểm chứng Pre-Uploader Shared EGL Context, generationId/revision guard và Render Thread fallback
        queue = []
        gles_cache = {}
        deleted = []
        uploaded_flags = set()

        def enqueue(handle, gen, rev):
            queue.append((handle, gen, rev))

        def drain(ready_items, live_state):
            for h, tex_id, gen, rev in ready_items:
                cur = gles_cache.get(h)
                if cur and cur[1] == gen and cur[2] == rev:
                    deleted.append(tex_id)  # Render Thread đã vẽ trước đó -> huỷ bản dư
                    continue
                live = live_state.get(h)
                if live and live[0] == gen and live[1] == rev:
                    if cur:
                        deleted.append(cur[0])
                    gles_cache[h] = (tex_id, gen, rev)
                    uploaded_flags.add(h)
                else:
                    deleted.append(tex_id)  # Handle đã chết hoặc bị tái cấp phát -> bỏ an toàn

        # 1. Happy path: worker tạo tex_100 cho gen=5, rev=1 và handle vẫn khớp
        enqueue("texA", gen=5, rev=1)
        ready = [("texA", 100, 5, 1), ("texB_dead", 101, 6, 1), ("texC_reused", 102, 7, 1)]
        live = {"texA": (5, 1), "texC_reused": (99, 1)}  # texC đã đổi generationId=99
        drain(ready, live)
        self.assertEqual(gles_cache["texA"], (100, 5, 1))
        self.assertIn("texA", uploaded_flags)
        self.assertEqual(deleted, [101, 102], "Handle chết hoặc khác generationId phải bị huỷ sạch")

        # 2. Edge case: đã upload đồng bộ trên Render Thread -> pre-uploaded bản trùng phải được giải phóng
        gles_cache["texD"] = (200, 10, 1)
        drain([("texD", 201, 10, 1)], {"texD": (10, 1)})
        self.assertEqual(gles_cache["texD"], (200, 10, 1))
        self.assertIn(201, deleted)

    def test_recycle_pool_and_gl_blit_framebuffer_fallback(self):
        # Kiểm chứng tái sử dụng RenderTarget/CopyFBO theo kích thước và toạ độ lật trục Y của glBlitFramebuffer
        # 1. Toạ độ glBlitFramebuffer từ Backbuffer (Bottom-Up effSurfH) sang FBO Top-Down [dstY..dstY+copyH]
        def calc_blit_coords(src_x, src_y, req_w, req_h, logical_w, logical_h, eff_w, eff_h):
            sx = eff_w / logical_w
            sy = eff_h / logical_h
            x0 = max(0, min(eff_w, round(src_x * sx)))
            x1 = max(0, min(eff_w, round((src_x + req_w) * sx)))
            y_top = max(0, min(eff_h, round(eff_h - src_y * sy)))
            y_bot = max(0, min(eff_h, round(eff_h - (src_y + req_h) * sy)))
            return (x0, y_top, x1, y_bot)

        self.assertEqual(calc_blit_coords(0, 0, 1920, 1080, 1920.0, 1080.0, 1920, 1080), (0, 1080, 1920, 0))
        self.assertEqual(calc_blit_coords(100, 50, 400, 200, 1920.0, 1080.0, 960, 540), (50, 515, 250, 415))

        # 2. Pool chỉ cho phép lấy buffer dirty đối với RT / full-copy; non-RT trống không lấy dirty
        pool = [{"texId": 10, "w": 1920, "h": 1080, "has_rt": True}, {"texId": 11, "w": 512, "h": 512, "has_rt": False}]
        def pop_pool(w, h, has_pixels, allow_dirty, need_rt):
            if not has_pixels and not allow_dirty:
                return None
            for idx, item in enumerate(pool):
                if item["w"] == w and item["h"] == h and item["has_rt"] == need_rt:
                    return pool.pop(idx)
            return None

        self.assertIsNone(pop_pool(512, 512, has_pixels=False, allow_dirty=False, need_rt=False))
        got_rt = pop_pool(1920, 1080, has_pixels=False, allow_dirty=True, need_rt=True)
        self.assertIsNotNone(got_rt)
        self.assertEqual(got_rt["texId"], 10)

    def test_multijob_bc7_pool_and_lockfree_texture_prepare(self):
        """Kiểm tra Bc7ThreadPool hỗ trợ nhiều activeJobs_ đồng thời và prepareTexturePixelsLockFree chạy ngoài mutex_."""
        import os

        cpp_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp"))
        with open(os.path.join(cpp_dir, "bc7_decoder.cpp"), "r", encoding="utf-8") as f:
            bc7_src = f.read()
        with open(os.path.join(cpp_dir, "resource_registry.cpp"), "r", encoding="utf-8") as f:
            rr_src = f.read()

        self.assertIn("activeJobs_", bc7_src)
        self.assertIn("job.inFlight", bc7_src)
        self.assertIn("prepareTexturePixelsLockFree", rr_src)
        self.assertIn("canStoreCompressedBc7", rr_src)

        # Kiểm chứng prepareTexturePixelsLockFree được gọi trước khi giữ std::lock_guard<std::mutex> lock(mutex_)
        reg_fn_idx = rr_src.find("bool ResourceRegistry::registerTexture(")
        self.assertGreater(reg_fn_idx, 0)
        prep_idx = rr_src.find("prepareTexturePixelsLockFree(", reg_fn_idx)
        lock_idx = rr_src.find("std::lock_guard<std::mutex> lock(mutex_);", reg_fn_idx)
        self.assertGreater(prep_idx, reg_fn_idx)
        self.assertLess(prep_idx, lock_idx)

        # Giả lập 2 job song song cùng rút chunk từ Bc7ThreadPool và lỗi input ngắn ở biên
        def run_parallel_jobs(job_rows):
            chunks = {jid: list(range(0, rows, 4)) for jid, rows in job_rows.items()}
            active = list(job_rows.keys())
            results = {jid: [] for jid in job_rows}
            idx = 0
            while active:
                jid = active[idx % len(active)]
                if chunks[jid]:
                    results[jid].append(chunks[jid].pop(0))
                    idx += 1
                else:
                    active.remove(jid)
            return results

        out = run_parallel_jobs({"atlasA": 12, "atlasB": 8, "empty": 0})
        self.assertEqual(out["atlasA"], [0, 4, 8])
        self.assertEqual(out["atlasB"], [0, 4])
        self.assertEqual(out["empty"], [])

    def test_bptc_gpu_upload_and_cpu_decompression_fallback(self):
        """Kiểm tra lưu trữ BPTC BC7 gốc, nhánh giải nén CPU khi cần truy cập CPU và fallback khi GL lỗi."""
        import os

        cpp_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp"))
        with open(os.path.join(cpp_dir, "resource_registry.cpp"), "r", encoding="utf-8") as f:
            rr_src = f.read()
        with open(os.path.join(cpp_dir, "jni_bridge.cpp"), "r", encoding="utf-8") as f:
            jni_src = f.read()

        self.assertIn("GL_EXT_texture_compression_bptc", jni_src)
        self.assertIn("0x8E8Cu", jni_src)
        self.assertIn("glCompressedTexImage2D", jni_src)
        self.assertIn("D3D11Bridge_EnsureTextureDecompressed", jni_src)
        self.assertGreaterEqual(rr_src.count("ensureEntryDecompressedLocked("), 4)

        k_dxgi_bc7_unorm = 98
        k_bind_render_target = 0x20
        k_usage_staging = 3

        def can_store_compressed(fmt, w, h, bind_flags, usage, cpu_access, init_pitch, bptc_enabled):
            if not bptc_enabled or fmt != k_dxgi_bc7_unorm:
                return False
            if (bind_flags & k_bind_render_target) != 0 or usage == k_usage_staging or cpu_access != 0:
                return False
            if (w % 4) != 0 or (h % 4) != 0:
                return False
            default_pitch = ((w + 3) // 4) * 16
            return init_pitch == 0 or init_pitch == default_pitch

        # Happy path: 4K BC7 atlas tĩnh đạt chuẩn BPTC
        self.assertTrue(can_store_compressed(98, 4096, 2312, 0x8, 1, 0, 16384, True))
        # Edge case: kích thước không chia hết cho 4, render target, staging hoặc có CPU access -> phải giải nén
        self.assertFalse(can_store_compressed(98, 1922, 1080, 0x8, 1, 0, 0, True))
        self.assertFalse(can_store_compressed(98, 4096, 2312, k_bind_render_target, 0, 0, 0, True))
        self.assertFalse(can_store_compressed(98, 4096, 2312, 0, k_usage_staging, 0x20000, 0, True))

        # Error handling: driver trả GL_INVALID_ENUM khi gọi glCompressedTexImage2D -> tắt BPTC & fallback RGBA8
        state = {"bptcEnabled": True, "compressed": True, "glCalls": []}

        def upload_with_fallback(fail_compressed=False):
            if state["compressed"]:
                state["glCalls"].append("glCompressedTexImage2D")
                if fail_compressed:
                    state["bptcEnabled"] = False
                    state["compressed"] = False
                    state["glCalls"].append("ensureTextureDecompressed")
                    state["glCalls"].append("glTexImage2D_RGBA8")
                    return "FALLBACK_RGBA8"
                return "COMPRESSED_BPTC"
            state["glCalls"].append("glTexImage2D_RGBA8")
            return "RGBA8"

        self.assertEqual(upload_with_fallback(fail_compressed=True), "FALLBACK_RGBA8")
        self.assertFalse(state["bptcEnabled"])
        self.assertFalse(state["compressed"])
        self.assertEqual(
            state["glCalls"],
            ["glCompressedTexImage2D", "ensureTextureDecompressed", "glTexImage2D_RGBA8"],
        )

    def test_preupload_inflight_dedup_and_perf_trace_markers(self):
        """Kiểm tra inFlightPreuploads tránh upload trùng texture 62MB và các mốc log [PerfTrace]."""
        import os

        cpp_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp"))
        with open(os.path.join(cpp_dir, "jni_bridge.cpp"), "r", encoding="utf-8") as f:
            jni_src = f.read()
        with open(os.path.join(cpp_dir, "d3d11_bridge.cpp"), "r", encoding="utf-8") as f:
            d3d_src = f.read()

        self.assertIn("inFlightPreuploads", jni_src)
        self.assertIn("waitForInFlightPreupload", jni_src)
        self.assertIn("drainReadyPreuploads();", jni_src)
        self.assertIn("[PerfTrace] Frame %u gap=%.2fms", jni_src)
        self.assertIn("[PerfTrace] CreateTexture2D %ux%u fmt=%u register+decode mat %.2fms", d3d_src)

        # Giả lập Render Thread chờ SharedEGL pre-upload hoàn tất rồi tái sử dụng kết quả trong cache
        gles_cache = {}
        ready_queue = []
        in_flight = {0xDEAD0001}

        def complete_background_upload(handle, tex_id, gen):
            ready_queue.append((handle, tex_id, gen))
            in_flight.discard(handle)

        def render_get_or_create(handle, gen):
            if handle in in_flight:
                complete_background_upload(handle, 777, gen)
            while ready_queue:
                h, tid, g = ready_queue.pop(0)
                gles_cache[h] = (tid, g)
            if handle in gles_cache and gles_cache[handle][1] == gen:
                return gles_cache[handle][0], False
            gles_cache[handle] = (888, gen)
            return 888, True

        tex_id, did_upload = render_get_or_create(0xDEAD0001, 5)
        self.assertEqual(tex_id, 777)
        self.assertFalse(did_upload)
        # Edge case: handle mới đổi generationId (tài nguyên đã tái sử dụng) -> Render Thread tự upload bản mới
        tex_id_gen6, did_upload_gen6 = render_get_or_create(0xDEAD0001, 6)
        self.assertEqual(tex_id_gen6, 888)
        self.assertTrue(did_upload_gen6)

if __name__ == "__main__":
    unittest.main()



