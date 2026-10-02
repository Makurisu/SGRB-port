import unittest

class TestWin32BridgeLogic(unittest.TestCase):
    def setUp(self):
        self.heap_allocations = {}
        self.virtual_allocations = {}

    def mock_heap_alloc(self, size, flags=0):
        if size == 0:
            size = 1
        # Giả lập cấp phát bộ nhớ
        ptr = id(object())
        self.heap_allocations[ptr] = size
        return ptr

    def mock_heap_free(self, ptr):
        if ptr is None or ptr == 0:
            return True
        if ptr in self.heap_allocations:
            del self.heap_allocations[ptr]
            return True
        # Bỏ qua con trỏ không hợp lệ, không crash
        return True

    def mock_heap_size(self, ptr):
        return self.heap_allocations.get(ptr, 0)

    def test_heap_alloc_and_free(self):
        ptr = self.mock_heap_alloc(1024)
        self.assertIn(ptr, self.heap_allocations)
        self.assertEqual(self.mock_heap_size(ptr), 1024)

        success = self.mock_heap_free(ptr)
        self.assertTrue(success)
        self.assertNotIn(ptr, self.heap_allocations)

    def test_heap_free_invalid_pointer(self):
        # Kiểm thử con trỏ rác không thuộc heap (như rsi = 0x48000085d9ebf52d)
        invalid_ptr = 0x48000085D9EBF52D
        success = self.mock_heap_free(invalid_ptr)
        self.assertTrue(success, "HeapFree không được gây crash khi gặp con trỏ lạ")

    def test_heap_free_null_pointer(self):
        success = self.mock_heap_free(None)
        self.assertTrue(success)
        success_zero = self.mock_heap_free(0)
        self.assertTrue(success_zero)

    def test_vfs_path_normalization(self):
        # Kiểm tra chuẩn hóa đường dẫn Windows sang Linux/Android VFS
        def normalize_win_path(path):
            s = path.replace("\\", "/")
            if len(s) >= 2 and s[1] == ":":
                s = s[2:]
            while s.startswith("/"):
                s = s[1:]
            return s

        self.assertEqual(normalize_win_path("C:\\Program Files\\SGRE\\title.pure.psb"), "Program Files/SGRE/title.pure.psb")
        self.assertEqual(normalize_win_path("data\\system\\font.bin"), "data/system/font.bin")
        self.assertEqual(normalize_win_path("movie/op.mp4"), "movie/op.mp4")
        self.assertEqual(normalize_win_path("\\savecfg.txt"), "savecfg.txt")

    def test_vfs_case_insensitive_resolution(self):
        # Mô phỏng tìm kiếm không phân biệt hoa thường
        mock_fs = {
            "data/title.pure.psb": "TITLE_CONTENT",
            "data/system/font.bin": "FONT_CONTENT",
            "movie/op.mp4": "VIDEO_CONTENT"
        }

        def resolve_case_insensitive(query_path):
            norm = query_path.replace("\\", "/").lower()
            for real_path in mock_fs:
                if real_path.lower() == norm:
                    return real_path
            return None

        self.assertEqual(resolve_case_insensitive("DATA\\Title.Pure.PSB"), "data/title.pure.psb")
        self.assertEqual(resolve_case_insensitive("data/SYSTEM/FONT.BIN"), "data/system/font.bin")
        self.assertEqual(resolve_case_insensitive("MOVIE/OP.MP4"), "movie/op.mp4")
        self.assertIsNone(resolve_case_insensitive("non_existent.bin"))

    def test_vfs_file_attributes(self):
        FILE_ATTRIBUTE_DIRECTORY = 0x10
        FILE_ATTRIBUTE_NORMAL = 0x80
        INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF

        mock_entries = {
            "data": FILE_ATTRIBUTE_DIRECTORY,
            "data/title.pure.psb": FILE_ATTRIBUTE_NORMAL,
        }

        self.assertEqual(mock_entries.get("data", INVALID_FILE_ATTRIBUTES), 0x10)
        self.assertEqual(mock_entries.get("data/title.pure.psb", INVALID_FILE_ATTRIBUTES), 0x80)
        self.assertEqual(mock_entries.get("missing.file", INVALID_FILE_ATTRIBUTES), 0xFFFFFFFF)

if __name__ == "__main__":
    unittest.main()

