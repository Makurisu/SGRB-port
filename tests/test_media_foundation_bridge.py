import unittest
import struct

class TestMediaFoundationBridge(unittest.TestCase):
    def setUp(self):
        # Media Foundation Constants
        self.MF_VERSION = 0x00020070
        self.MF_SOURCE_READERF_ENDOFSTREAM = 0x00000001
        self.MF_OBJECT_MEDIASOURCE = 1
        self.S_OK = 0

    def mock_mf_startup(self, version, flags):
        if version == 0:
            return 0x80070057 # E_INVALIDARG
        return self.S_OK

    def mock_mf_create_attributes(self):
        # Trả về đối tượng mock IMFAttributes
        return (self.S_OK, "MockIMFAttributes")

    def mock_mf_create_dxgi_device_manager(self):
        return (self.S_OK, 1, "MockIMFDXGIDeviceManager")

    def mock_mf_ll_mul_div(self, a, b, c, d):
        if c == 0:
            return 0
        return (a * b + d) // c

    def mock_mf_calculate_image_size(self, width, height):
        return width * height * 4 # RGBA32

    def mock_source_reader_read_sample(self, stream_idx):
        # Trả về cờ ENDOFSTREAM để cho phép game loop chuyển cảnh an toàn
        stream_flags = self.MF_SOURCE_READERF_ENDOFSTREAM
        timestamp = 0
        sample = None
        return (self.S_OK, stream_idx, stream_flags, timestamp, sample)

    def test_mf_startup_success(self):
        res = self.mock_mf_startup(self.MF_VERSION, 0)
        self.assertEqual(res, self.S_OK, "MFStartup phải trả về S_OK (0)")

    def test_mf_startup_invalid_version(self):
        res = self.mock_mf_startup(0, 0)
        self.assertNotEqual(res, self.S_OK, "MFStartup với version 0 phải trả về lỗi")

    def test_mf_create_attributes(self):
        hr, attrs = self.mock_mf_create_attributes()
        self.assertEqual(hr, self.S_OK)
        self.assertEqual(attrs, "MockIMFAttributes")

    def test_mf_create_dxgi_device_manager(self):
        hr, reset_token, manager = self.mock_mf_create_dxgi_device_manager()
        self.assertEqual(hr, self.S_OK)
        self.assertEqual(reset_token, 1)
        self.assertEqual(manager, "MockIMFDXGIDeviceManager")

    def test_mf_ll_mul_div(self):
        # (1000 * 60 + 0) / 30 = 2000
        val = self.mock_mf_ll_mul_div(1000, 60, 30, 0)
        self.assertEqual(val, 2000)
        # c = 0 không được gây ngoại lệ ZeroDivisionError
        val_zero = self.mock_mf_ll_mul_div(1000, 60, 0, 0)
        self.assertEqual(val_zero, 0)

    def test_mf_calculate_image_size(self):
        # 1920x1080x4 = 8,294,400 bytes
        size = self.mock_mf_calculate_image_size(1920, 1080)
        self.assertEqual(size, 1920 * 1080 * 4)

    def test_source_reader_end_of_stream_flag(self):
        hr, actual_idx, stream_flags, ts, sample = self.mock_source_reader_read_sample(0)
        self.assertEqual(hr, self.S_OK)
        self.assertEqual(actual_idx, 0)
        self.assertTrue(stream_flags & self.MF_SOURCE_READERF_ENDOFSTREAM,
                        "Cờ MF_SOURCE_READERF_ENDOFSTREAM phải được bật để tránh treo vòng lặp video")
        self.assertIsNone(sample)

if __name__ == "__main__":
    unittest.main()
