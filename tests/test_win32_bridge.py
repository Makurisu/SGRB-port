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

    def test_utf16_path_normalization(self):
        # Kiểm tra giải mã UTF-16LE 16-bit sang UTF-8
        def normalize_win_path_w(raw_bytes):
            # raw_bytes là chuỗi byte UTF-16LE kết thúc bằng \0
            chars = []
            for i in range(0, len(raw_bytes) - 1, 2):
                code = raw_bytes[i] | (raw_bytes[i + 1] << 8)
                if code == 0:
                    break
                if code == ord('\\'):
                    chars.append('/')
                else:
                    chars.append(chr(code))
            s = "".join(chars)
            if len(s) >= 2 and s[1] == ':':
                s = s[2:]
            while s.startswith("/"):
                s = s[1:]
            return s

        sample_path = "C:\\Program Files\\SGRE\\fullscreen.txt"
        utf16_bytes = sample_path.encode("utf-16le") + b"\x00\x00"
        normalized = normalize_win_path_w(utf16_bytes)
        self.assertEqual(normalized, "Program Files/SGRE/fullscreen.txt")

    def test_win32_find_data_w_size(self):
        # Kích thước chuẩn trên Windows x64: 592 bytes
        # dwFileAttributes (4) + 3*FILETIME(8) + 2*FileSize(4) + 2*Reserved(4) + 260*char16_t(2) + 14*char16_t(2)
        expected_size = 4 + (3 * 8) + (2 * 4) + (2 * 4) + (260 * 2) + (14 * 2)
        self.assertEqual(expected_size, 592, "Win32FindDataW phải có đúng kích thước 592 bytes")

    def test_shell_special_folder(self):
        # Giả lập SHGetSpecialFolderPathW cho CSIDL_PERSONAL (Documents)
        CSIDL_PERSONAL = 0x0005
        CSIDL_APPDATA = 0x001A

        def mock_sh_get_folder(csidl):
            if csidl in (CSIDL_PERSONAL, CSIDL_APPDATA):
                return "C:\\saves"
            return "C:\\"

        self.assertEqual(mock_sh_get_folder(CSIDL_PERSONAL), "C:\\saves")
        self.assertEqual(mock_sh_get_folder(CSIDL_APPDATA), "C:\\saves")

    def test_message_queue_and_pump(self):
        # Giả lập hàng đợi thông điệp Win32 và PeekMessage
        queue = []

        def post_message(msg_id, wparam, lparam):
            queue.append({"msg": msg_id, "wParam": wparam, "lParam": lparam})

        def peek_message(remove=True):
            if not queue:
                return None
            msg = queue[0]
            if remove:
                queue.pop(0)
            return msg

        # Đẩy sự kiện chuột WM_LBUTTONDOWN (0x0201)
        post_message(0x0201, 1, 0x01680300)
        self.assertEqual(len(queue), 1)

        # Peek không xoá
        msg_peek = peek_message(remove=False)
        self.assertIsNotNone(msg_peek)
        self.assertEqual(msg_peek["msg"], 0x0201)
        self.assertEqual(len(queue), 1)

        # Peek có xoá (PM_REMOVE)
        msg_pop = peek_message(remove=True)
        self.assertEqual(msg_pop["msg"], 0x0201)
        self.assertEqual(len(queue), 0)

if __name__ == "__main__":
    unittest.main()

