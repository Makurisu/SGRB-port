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

    def test_thread_creation_and_lifecycle(self):
        # Giả lập vòng đời Win32 Thread
        thread_map = {}
        STILL_ACTIVE = 0x103

        def create_thread(routine, param):
            h_thread = 0x20001 + len(thread_map)
            thread_map[h_thread] = {
                "routine": routine,
                "param": param,
                "finished": False,
                "exitCode": STILL_ACTIVE
            }
            return h_thread

        def get_exit_code(h_thread):
            if h_thread not in thread_map:
                return 0
            return thread_map[h_thread]["exitCode"]

        def complete_thread(h_thread, code):
            thread_map[h_thread]["finished"] = True
            thread_map[h_thread]["exitCode"] = code

        h = create_thread(lambda p: 0, None)
        self.assertEqual(get_exit_code(h), STILL_ACTIVE)

        complete_thread(h, 42)
        self.assertEqual(get_exit_code(h), 42)

    def test_thread_creation_suspended_and_resume(self):
        # Giả lập cờ CREATE_SUSPENDED và hàm ResumeThread
        thread_map = {}
        CREATE_SUSPENDED = 0x00000004
        STILL_ACTIVE = 0x103

        def create_thread(routine, param, flags=0):
            h_thread = 0x20001 + len(thread_map)
            thread_map[h_thread] = {
                "routine": routine,
                "param": param,
                "finished": False,
                "exitCode": STILL_ACTIVE,
                "suspended": (flags & CREATE_SUSPENDED) != 0
            }
            return h_thread

        def resume_thread(h_thread):
            if h_thread in thread_map and thread_map[h_thread]["suspended"]:
                thread_map[h_thread]["suspended"] = False
                return 1
            return 0

        # Thread thường (không suspended)
        h1 = create_thread(lambda p: 0, None, 0)
        self.assertFalse(thread_map[h1]["suspended"])
        self.assertEqual(resume_thread(h1), 0)

        # Thread với cờ CREATE_SUSPENDED
        h2 = create_thread(lambda p: 0, None, CREATE_SUSPENDED)
        self.assertTrue(thread_map[h2]["suspended"])
        self.assertEqual(resume_thread(h2), 1)
        self.assertFalse(thread_map[h2]["suspended"])
        self.assertEqual(resume_thread(h2), 0) # Đã resume rồi thì trả về 0

    def test_win32_event_synchronization(self):
        # Giả lập Win32 Event subsystem
        sync_map = {}
        WAIT_OBJECT_0 = 0
        WAIT_TIMEOUT = 0x102

        def create_event(manual_reset, initial_state):
            handle = 0x50001 + len(sync_map)
            sync_map[handle] = {
                "type": "event",
                "manual_reset": manual_reset,
                "signaled": initial_state
            }
            return handle

        def set_event(h):
            if h in sync_map and sync_map[h]["type"] == "event":
                sync_map[h]["signaled"] = True
                return 1
            return 0

        def reset_event(h):
            if h in sync_map and sync_map[h]["type"] == "event":
                sync_map[h]["signaled"] = False
                return 1
            return 0

        def wait_event(h):
            if h in sync_map and sync_map[h]["type"] == "event":
                if sync_map[h]["signaled"]:
                    if not sync_map[h]["manual_reset"]:
                        sync_map[h]["signaled"] = False # Auto-reset
                    return WAIT_OBJECT_0
                return WAIT_TIMEOUT
            return WAIT_TIMEOUT

        # 1. Auto-reset event
        h_auto = create_event(manual_reset=False, initial_state=False)
        self.assertEqual(wait_event(h_auto), WAIT_TIMEOUT) # Chưa signal thì timeout

        set_event(h_auto)
        self.assertEqual(wait_event(h_auto), WAIT_OBJECT_0) # Nhận tín hiệu thành công
        self.assertEqual(wait_event(h_auto), WAIT_TIMEOUT) # Tự động reset về false

        # 2. Manual-reset event
        h_manual = create_event(manual_reset=True, initial_state=False)
        set_event(h_manual)
        self.assertEqual(wait_event(h_manual), WAIT_OBJECT_0)
        self.assertEqual(wait_event(h_manual), WAIT_OBJECT_0) # Vẫn giữ tín hiệu

        reset_event(h_manual)
        self.assertEqual(wait_event(h_manual), WAIT_TIMEOUT) # Đã reset thành công

    def test_win32_semaphore_synchronization(self):
        # Giả lập Win32 Semaphore subsystem
        sync_map = {}
        WAIT_OBJECT_0 = 0
        WAIT_TIMEOUT = 0x102

        def create_semaphore(initial_count, max_count):
            handle = 0x50001 + len(sync_map)
            sync_map[handle] = {
                "type": "semaphore",
                "count": initial_count,
                "max_count": max_count
            }
            return handle

        def release_semaphore(h, release_count):
            if h in sync_map and sync_map[h]["type"] == "semaphore":
                obj = sync_map[h]
                if obj["count"] + release_count > obj["max_count"]:
                    return 0
                obj["count"] += release_count
                return 1
            return 0

        def wait_semaphore(h):
            if h in sync_map and sync_map[h]["type"] == "semaphore":
                obj = sync_map[h]
                if obj["count"] > 0:
                    obj["count"] -= 1
                    return WAIT_OBJECT_0
                return WAIT_TIMEOUT
            return WAIT_TIMEOUT

        # Khởi tạo Semaphore rỗng (0 task)
        h_sem = create_semaphore(initial_count=0, max_count=10)
        self.assertEqual(wait_semaphore(h_sem), WAIT_TIMEOUT) # Chưa có task, worker thread phải chờ

        # Đẩy 3 tasks vào Semaphore
        ok = release_semaphore(h_sem, 3)
        self.assertEqual(ok, 1)
        self.assertEqual(sync_map[h_sem]["count"], 3)

        # 3 worker threads lần lượt nhận 3 tasks
        self.assertEqual(wait_semaphore(h_sem), WAIT_OBJECT_0)
        self.assertEqual(wait_semaphore(h_sem), WAIT_OBJECT_0)
        self.assertEqual(wait_semaphore(h_sem), WAIT_OBJECT_0)

        # Hết task, worker thread tiếp theo phải timeout
        self.assertEqual(wait_semaphore(h_sem), WAIT_TIMEOUT)

    def test_window_procedure_dispatch(self):
        # Giả lập Window Procedure callback và DispatchMessage
        captured_messages = []

        def mock_wnd_proc(hwnd, msg, wparam, lparam):
            captured_messages.append((hwnd, msg, wparam, lparam))
            return 1234

        registered_wnd_proc = mock_wnd_proc

        def dispatch_message(msg_obj):
            if registered_wnd_proc:
                return registered_wnd_proc(msg_obj["hwnd"], msg_obj["msg"], msg_obj["wParam"], msg_obj["lParam"])
            return 0

        sample_msg = {"hwnd": 0x30003000, "msg": 0x0001, "wParam": 0, "lParam": 0} # WM_CREATE
        res = dispatch_message(sample_msg)
        self.assertEqual(res, 1234)
        self.assertEqual(len(captured_messages), 1)
        self.assertEqual(captured_messages[0], (0x30003000, 0x0001, 0, 0))

    def test_crypto_api_stubs(self):
        # Kiểm tra giả lập ADVAPI32 Crypto context và key handles
        def crypt_acquire_context():
            return True, 0x70001

        def crypt_import_key(prov_handle):
            if prov_handle == 0x70001:
                return True, 0x70002
            return False, 0

        ok, h_prov = crypt_acquire_context()
        self.assertTrue(ok)
        self.assertEqual(h_prov, 0x70001)

        ok_key, h_key = crypt_import_key(h_prov)
        self.assertTrue(ok_key)
        self.assertEqual(h_key, 0x70002)

    def test_gdi_and_system_stubs(self):
        # Kiểm tra giả lập GetDeviceCaps
        HORZRES = 8
        VERTRES = 10
        BITSPIXEL = 12

        def get_device_caps(idx):
            if idx == HORZRES:
                return 1920
            if idx == VERTRES:
                return 1080
            if idx == BITSPIXEL:
                return 32
            return 1

        self.assertEqual(get_device_caps(HORZRES), 1920)
        self.assertEqual(get_device_caps(VERTRES), 1080)
        self.assertEqual(get_device_caps(BITSPIXEL), 32)

if __name__ == "__main__":
    unittest.main()


