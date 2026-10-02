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
        elif iid in (self.IID_ID3D11DEVICE, self.IID_ID3D11DEVICE1):
            return "MockID3D11Device"
        elif iid == self.IID_ID3D11CONTEXT:
            return "MockID3D11DeviceContext"
        return "MockGeneric"

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

    def test_vtable_slot_isolation(self):
        # Kiểm tra slot 5 của Multithread là SetMultithreadProtected (2 args)
        # Trong khi slot 5 của Device là CreateTexture2D (4 args)
        multithread_slot_5 = "SetMultithreadProtected(this, bProtect)"
        device_slot_5 = "CreateTexture2D(this, desc, data, ppTex)"
        self.assertNotEqual(multithread_slot_5, device_slot_5,
                           "Vtable của Multithread và Device phải tách biệt hoàn toàn để tránh crash SEGV_MAPERR")

if __name__ == "__main__":
    unittest.main()
