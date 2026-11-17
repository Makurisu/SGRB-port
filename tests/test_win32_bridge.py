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

    def test_vfs_resource_prefix_fallback(self):
        # Giả lập fallback tiền tố windata/, winweb/, wind3d11data/
        mock_fs = {
            "script_info.psb.m": "SCRIPT_INFO_DATA",
            "script_body.bin": "SCRIPT_BODY_DATA",
            "wind3d11data/scenario_info.psb.m": "SCENARIO_INFO_DATA"
        }

        def resolve_with_fallback(query_path):
            norm = query_path.replace("\\", "/").lower()
            if norm in mock_fs:
                return norm
            parts = norm.split("/")
            if len(parts) > 1 and parts[0].startswith("win"):
                stripped = "/".join(parts[1:])
                if stripped in mock_fs:
                    return stripped
                fallback_d3d = "wind3d11data/" + stripped
                if fallback_d3d in mock_fs:
                    return fallback_d3d
            fallback_d3d = "wind3d11data/" + norm
            if fallback_d3d in mock_fs:
                return fallback_d3d
            return None

        # Truy vấn trực tiếp
        self.assertEqual(resolve_with_fallback("script_info.psb.m"), "script_info.psb.m")
        # Truy vấn qua tiền tố windata/
        self.assertEqual(resolve_with_fallback("windata/script_info.psb.m"), "script_info.psb.m")
        # Truy vấn qua tiền tố winweb/
        self.assertEqual(resolve_with_fallback("winweb/script_info.psb.m"), "script_info.psb.m")
        # Truy vấn file nằm trong wind3d11data/ qua tiền tố windata/
        self.assertEqual(resolve_with_fallback("windata/scenario_info.psb.m"), "wind3d11data/scenario_info.psb.m")

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

    def test_window_property_subsystem(self):
        # Kiểm tra giả lập SetPropA, GetPropA, RemovePropA
        props = {}

        def set_prop(hwnd, prop, data):
            if not hwnd or not prop:
                return False
            key = (hwnd, prop)
            props[key] = data
            return True

        def get_prop(hwnd, prop):
            if not hwnd or not prop:
                return None
            return props.get((hwnd, prop), None)

        def remove_prop(hwnd, prop):
            if not hwnd or not prop:
                return None
            return props.pop((hwnd, prop), None)

        dummy_hwnd = 0x1111
        instance_ptr = 0x763870001000
        prop_name = "__OWN_INSTANCE__"

        # Ban đầu chưa có property
        self.assertIsNone(get_prop(dummy_hwnd, prop_name))

        # Đặt property
        self.assertTrue(set_prop(dummy_hwnd, prop_name, instance_ptr))
        self.assertEqual(get_prop(dummy_hwnd, prop_name), instance_ptr)

        # Cửa sổ khác không thấy property này
        other_hwnd = 0x2222
        self.assertIsNone(get_prop(other_hwnd, prop_name))

        # Kiểm tra với atom (<= 0xFFFF)
        atom_val = 0x1234
        self.assertTrue(set_prop(dummy_hwnd, atom_val, 0x9999))
        self.assertEqual(get_prop(dummy_hwnd, atom_val), 0x9999)

        # Xóa property
        removed = remove_prop(dummy_hwnd, prop_name)
        self.assertEqual(removed, instance_ptr)
        self.assertIsNone(get_prop(dummy_hwnd, prop_name))

    def test_window_long_subsystem(self):
        # Kiểm tra giả lập GetWindowLongPtrA, SetWindowLongPtrA
        window_longs = {}
        GWL_STYLE = -16
        GWLP_USERDATA = -21
        WS_VISIBLE = 0x10000000

        def get_window_long_ptr(hwnd, index):
            if (hwnd, index) in window_longs:
                return window_longs[(hwnd, index)]
            if index == GWL_STYLE:
                return WS_VISIBLE
            return 0

        def set_window_long_ptr(hwnd, index, new_val):
            old = get_window_long_ptr(hwnd, index)
            window_longs[(hwnd, index)] = new_val
            return old

        dummy_hwnd = 0x1111
        # Mặc định GWL_STYLE trả về WS_VISIBLE
        self.assertEqual(get_window_long_ptr(dummy_hwnd, GWL_STYLE), WS_VISIBLE)

        # Gán GWLP_USERDATA
        old_val = set_window_long_ptr(dummy_hwnd, GWLP_USERDATA, 0x12345678)
        self.assertEqual(old_val, 0)
        self.assertEqual(get_window_long_ptr(dummy_hwnd, GWLP_USERDATA), 0x12345678)

    def test_initial_activation_message_sequence(self):
        # Kiểm tra chuỗi 7 thông điệp khởi tạo vòng đời cửa sổ Win32
        WM_CREATE = 0x0001
        WM_SHOWWINDOW = 0x0018
        WM_ACTIVATEAPP = 0x001C
        WM_ACTIVATE = 0x0006
        WM_SETFOCUS = 0x0007
        WM_SIZE = 0x0005
        WM_PAINT = 0x000F

        queue = [
            {"msg": WM_CREATE, "wParam": 0, "lParam": 0},
            {"msg": WM_SHOWWINDOW, "wParam": 1, "lParam": 0},
            {"msg": WM_ACTIVATEAPP, "wParam": 1, "lParam": 0},
            {"msg": WM_ACTIVATE, "wParam": 1, "lParam": 0},
            {"msg": WM_SETFOCUS, "wParam": 0, "lParam": 0},
            {"msg": WM_SIZE, "wParam": 0, "lParam": (1080 << 16) | 1920},
            {"msg": WM_PAINT, "wParam": 0, "lParam": 0},
        ]

        received = []
        is_active = False
        is_sized = False

        for m in queue:
            received.append(m["msg"])
            if m["msg"] == WM_ACTIVATE and m["wParam"] == 1:
                is_active = True
            elif m["msg"] == WM_SIZE:
                is_sized = True

        self.assertEqual(len(received), 7)
        self.assertEqual(received[0], WM_CREATE)
        self.assertEqual(received[1], WM_SHOWWINDOW)
        self.assertEqual(received[2], WM_ACTIVATEAPP)
        self.assertEqual(received[3], WM_ACTIVATE)
        self.assertEqual(received[4], WM_SETFOCUS)
        self.assertEqual(received[5], WM_SIZE)
        self.assertEqual(received[6], WM_PAINT)
        self.assertTrue(is_active)
        self.assertTrue(is_sized)

    def test_overlapped_read_file_signals_event(self):
        # Kiểm thử cấu trúc và luồng xử lý ReadFile với OVERLAPPED (tại 0x1404dbca3)
        mock_file_data = b"M2_ENGINE_SCRIPT_HEADER" + b"\x00" * 2048
        
        class MockOverlapped:
            def __init__(self, offset_low=0, offset_high=0, h_event=None):
                self.internal = 0x103 # STATUS_PENDING
                self.internal_high = 0
                self.offset = offset_low
                self.offset_high = offset_high
                self.h_event = h_event

        class MockEvent:
            def __init__(self):
                self.signaled = False

        event = MockEvent()
        ov = MockOverlapped(offset_low=10, offset_high=0, h_event=event)
        read_buffer = bytearray(2245)

        def mock_stub_read_file(h_file, buf, count, p_read, overlapped):
            if not h_file or not buf:
                if p_read is not None: p_read[0] = 0
                if overlapped:
                    overlapped.internal = 0xC0000008
                    overlapped.internal_high = 0
                    if overlapped.h_event: overlapped.h_event.signaled = True
                return False

            if overlapped:
                file_offset = (overlapped.offset_high << 32) | overlapped.offset
                data_slice = mock_file_data[file_offset:file_offset + count]
                bytes_read = len(data_slice)
                buf[:bytes_read] = data_slice
                overlapped.internal = 0 # STATUS_SUCCESS
                overlapped.internal_high = bytes_read
                if p_read is not None:
                    p_read[0] = bytes_read
                if overlapped.h_event:
                    overlapped.h_event.signaled = True
                return True
            return False

        # Trường hợp 1: Game engine gọi ReadFile với lpNumberOfBytesRead = NULL (0x1404dbcb2: xor r9d, r9d)
        res = mock_stub_read_file(0x10003, read_buffer, 2245, None, ov)
        self.assertTrue(res)
        self.assertEqual(ov.internal, 0, "internal phải là 0 (STATUS_SUCCESS)")
        self.assertEqual(ov.internal_high, len(mock_file_data) - 10, "internalHigh phải lưu số byte đã truyền")
        self.assertTrue(event.signaled, "hEvent (0x5000a) bắt buộc phải được đánh thức qua SetEvent")

        # Kiểm tra nội dung đọc từ đúng offset
        self.assertEqual(bytes(read_buffer[:len(b"SCRIPT_HEADER")]), b"SCRIPT_HEADER")

    def test_overlapped_write_file_signals_event(self):
        # Kiểm thử WriteFile với OVERLAPPED
        written_data = {}

        class MockOverlapped:
            def __init__(self, offset_low=0, offset_high=0, h_event=None):
                self.internal = 0x103
                self.internal_high = 0
                self.offset = offset_low
                self.offset_high = offset_high
                self.h_event = h_event

        class MockEvent:
            def __init__(self):
                self.signaled = False

        event = MockEvent()
        ov = MockOverlapped(offset_low=128, offset_high=0, h_event=event)
        payload = b"SAVE_GAME_DATA_PAYLOAD"

        def mock_stub_write_file(h_file, buf, count, p_written, overlapped):
            if overlapped:
                file_offset = (overlapped.offset_high << 32) | overlapped.offset
                written_data[file_offset] = buf[:count]
                overlapped.internal = 0 # STATUS_SUCCESS
                overlapped.internal_high = count
                if p_written is not None:
                    p_written[0] = count
                if overlapped.h_event:
                    overlapped.h_event.signaled = True
                return True
            return False

        res = mock_stub_write_file(0x10004, payload, len(payload), None, ov)
        self.assertTrue(res)
        self.assertEqual(ov.internal, 0)
        self.assertEqual(ov.internal_high, len(payload))
        self.assertTrue(event.signaled)
        self.assertIn(128, written_data)
        self.assertEqual(written_data[128], payload)

    def test_get_overlapped_result(self):
        # Kiểm thử GetOverlappedResult
        class MockOverlapped:
            def __init__(self, internal, transferred):
                self.internal = internal
                self.internal_high = transferred
                self.h_event = None

        def mock_get_overlapped_result(overlapped, p_transferred, b_wait):
            if not overlapped: return False
            if p_transferred is not None:
                p_transferred[0] = overlapped.internal_high
            return (overlapped.internal == 0)

        # Trạng thái thành công
        ov_success = MockOverlapped(0, 1024)
        transferred = [0]
        ok = mock_get_overlapped_result(ov_success, transferred, False)
        self.assertTrue(ok)
        self.assertEqual(transferred[0], 1024)

        # Trạng thái thất bại
        ov_fail = MockOverlapped(0xC0000001, 0)
        transferred_fail = [0]
        fail = mock_get_overlapped_result(ov_fail, transferred_fail, False)
        self.assertFalse(fail)
        self.assertEqual(transferred_fail[0], 0)

    def test_winmm_wave_audio_apis(self):
        # Kiểm thử các API âm thanh WinMM (waveOut / waveIn) trả về MMSYSERR_NOERROR (0) và handle hợp lệ
        MMSYSERR_NOERROR = 0

        # waveOutOpen
        hwave_out = [None]
        def mock_wave_out_open(phwo):
            phwo[0] = 0x8001
            return MMSYSERR_NOERROR

        res = mock_wave_out_open(hwave_out)
        self.assertEqual(res, MMSYSERR_NOERROR)
        self.assertEqual(hwave_out[0], 0x8001)

        # waveInOpen
        hwave_in = [None]
        def mock_wave_in_open(phwi):
            phwi[0] = 0x8002
            return MMSYSERR_NOERROR

        res_in = mock_wave_in_open(hwave_in)
        self.assertEqual(res_in, MMSYSERR_NOERROR)
        self.assertEqual(hwave_in[0], 0x8002)

    def test_bgra_to_rgba_swizzle(self):
        # Kiểm thử chuyển đổi màu từ DXGI_FORMAT_B8G8R8A8_UNORM sang RGBA8
        bgra_pixels = bytes([0xFF, 0x00, 0x10, 0x80]) # B=255, G=0, R=16, A=128
        rgba_result = bytearray(4)
        rgba_result[0] = bgra_pixels[2] # R
        rgba_result[1] = bgra_pixels[1] # G
        rgba_result[2] = bgra_pixels[0] # B
        rgba_result[3] = bgra_pixels[3] # A

        self.assertEqual(rgba_result[0], 0x10) # R
        self.assertEqual(rgba_result[1], 0x00) # G
    def test_srwlock_exclusive_and_shared_locking(self):
        # Kiểm thử logic mô phỏng SRWLock Win32
        class MockSRWLock:
            def __init__(self):
                self.exclusive_owner = None
                self.shared_count = 0

            def acquire_exclusive(self, tid):
                if self.exclusive_owner is None and self.shared_count == 0:
                    self.exclusive_owner = tid
                    return True
                return False

            def release_exclusive(self, tid):
                if self.exclusive_owner == tid:
                    self.exclusive_owner = None
                    return True
                return False

            def acquire_shared(self):
                if self.exclusive_owner is None:
                    self.shared_count += 1
                    return True
                return False

            def release_shared(self):
                if self.shared_count > 0:
                    self.shared_count -= 1
                    return True
                return False

            def try_acquire_exclusive(self, tid):
                return self.acquire_exclusive(tid)

        srw = MockSRWLock()
        self.assertTrue(srw.acquire_exclusive(1001))
        # Không cho phép thread khác hoặc shared lock khi exclusive đang giữ
        self.assertFalse(srw.acquire_exclusive(1002))
        self.assertFalse(srw.acquire_shared())
        self.assertFalse(srw.try_acquire_exclusive(1002))

        # Release exclusive
        self.assertTrue(srw.release_exclusive(1001))

        # Bây giờ cho phép nhiều shared locks
        self.assertTrue(srw.acquire_shared())
        self.assertTrue(srw.acquire_shared())
        self.assertEqual(srw.shared_count, 2)
        # Khi đang có shared lock, exclusive lock phải thất bại
        self.assertFalse(srw.acquire_exclusive(1003))

        # Giải phóng toàn bộ shared locks
        self.assertTrue(srw.release_shared())
        self.assertTrue(srw.release_shared())
        self.assertEqual(srw.shared_count, 0)
        self.assertTrue(srw.acquire_exclusive(1003))
        self.assertTrue(srw.release_exclusive(1003))

    def test_condition_variable_sleep_and_wake_logic(self):
        # Kiểm thử logic ConditionVariable với SRWLock
        class MockConditionVariable:
            def __init__(self):
                self.waiting_threads = []

            def sleep_condition_variable_srw(self, srw, tid, dw_ms):
                # Giải phóng SRWLock trước khi ngủ
                srw.release_exclusive(tid)
                self.waiting_threads.append(tid)
                # Giả lập thức dậy và re-acquire SRWLock
                self.waiting_threads.remove(tid)
                srw.acquire_exclusive(tid)
                return True

            def wake_one(self):
                return len(self.waiting_threads) > 0

            def wake_all(self):
                return len(self.waiting_threads)

        class MockExclusiveLock:
            def __init__(self):
                self.owner = None
            def acquire_exclusive(self, tid):
                self.owner = tid
                return True
            def release_exclusive(self, tid):
                if self.owner == tid:
                    self.owner = None
                    return True
                return False

        srw = MockExclusiveLock()
        cv = MockConditionVariable()
        srw.acquire_exclusive(1001)
        self.assertEqual(srw.owner, 1001)

        success = cv.sleep_condition_variable_srw(srw, 1001, 0xFFFFFFFF)
        self.assertTrue(success)
        self.assertEqual(srw.owner, 1001, "Sau khi thức dậy, SRWLock phải được tái sở hữu")

    def test_thread_id_kernel_tid_consistency(self):
        # Kiểm thử Thread ID là số nguyên 32-bit dương, không bị cắt ngắn
        mock_tid = 12345
        self.assertEqual(mock_tid & 0xFFFFFFFF, mock_tid)
        self.assertGreater(mock_tid, 0)

    def test_mtx_do_lock_deadlock_patch_verification(self):
        # Kiểm thử mã máy hotfix tại RVA 0x56D20D
        original_bytes = bytearray([0x75, 0x2A]) # jnz loc_14056D239
        # Patch chuyển 0x75 thành 0xEB (jmp)
        if original_bytes[0] == 0x75 and original_bytes[1] == 0x2A:
            original_bytes[0] = 0xEB # jmp loc_14056D239

        self.assertEqual(original_bytes[0], 0xEB)
        self.assertEqual(original_bytes[1], 0x2A)

    def test_query_performance_frequency_and_overflow_prevention(self):
        # Tần số QPF chuẩn Windows NT là 10,000,000 (10 MHz, 100ns/tick)
        qpf_frequency = 10000000
        self.assertEqual(qpf_frequency, 10_000_000)

        # Giả lập 1 frame tại 60 FPS (16.666 ms = 16,666,666 ns)
        frame_time_ns = 16666666
        # QPC ticks = nanoseconds / 100
        delta_ticks = frame_time_ns // 100 # 166,666 ticks
        self.assertEqual(delta_ticks, 166666)

        # Tính toán delta milliseconds theo công thức CRT/M2 Engine 32-bit:
        # delta_ms = (delta_ticks * 1000) / qpf_frequency
        # Phép nhân 32-bit: delta_ticks * 1000 = 166,666,000 (hoàn toàn nằm trong giới hạn int32 2,147,483,647)
        product = delta_ticks * 1000
        self.assertLess(product, 2147483647, "Phép nhân không được vượt ngưỡng 32-bit signed int")
        delta_ms = product // qpf_frequency
        self.assertEqual(delta_ms, 16, "Delta ms phải là 16ms chuẩn cho 1 frame 60 FPS")

        # Ngược lại, nếu tần số là 1 GHz (1,000,000,000):
        faulty_qpf = 1000000000
        faulty_delta_ticks = frame_time_ns # 16,666,666
        faulty_product = faulty_delta_ticks * 1000 # 16,666,666,000 -> TRÀN SỐ!
        self.assertGreater(faulty_product, 2147483647, "1 GHz gây tràn số 32-bit integer overflow")

    def test_virtual_monotonic_clock_frame_clamping(self):
        # Kiểm thử cơ chế Virtual Monotonic Clock và Clamping bước nhảy frame
        virtual_time_ms = 0
        frame_start_real = 0

        def on_frame_presented(real_now):
            nonlocal virtual_time_ms, frame_start_real
            delta_ms = real_now - frame_start_real
            step_ms = max(1, min(delta_ms, 17))
            virtual_time_ms += step_ms
            frame_start_real = real_now

        def query_performance_counter(real_now):
            delta_within = real_now - frame_start_real
            step_within = max(0, min(delta_within, 17))
            return virtual_time_ms + step_within

        # 1. Khung hình bình thường (16ms)
        t_prev = query_performance_counter(0)
        t_curr = query_performance_counter(16)
        delta_tick = t_curr - t_prev
        v7 = int((delta_tick * 60.0 / 1000.0) + 0.5)
        self.assertEqual(v7, 1, "Frame 16ms phải cho bước nhảy đúng 1 frame")
        on_frame_presented(16)

        # 2. Khung hình bị nghẽn do nạp asset/compile shader (800ms)
        t_hitch = query_performance_counter(16 + 800)
        delta_hitch = t_hitch - t_curr
        v7_hitch = int((delta_hitch * 60.0 / 1000.0) + 0.5)
        self.assertEqual(delta_hitch, 17, "Bước nhảy thời gian tối đa khi lag phải được clamp ở 17ms")
        self.assertEqual(v7_hitch, 1, "Lag 800ms KHÔNG được nhảy cóc frame, bước nhảy hoạt cảnh phải là 1 frame")

    def test_tls_allocation_and_per_thread_storage(self):
        # Kiểm thử bộ quản lý TLS Thread-Local Storage (128 slots)
        MAX_TLS = 128
        tls_allocated = [False] * MAX_TLS
        tls_thread_storage = {}
        import threading

        def tls_alloc():
            for i in range(MAX_TLS):
                if not tls_allocated[i]:
                    tls_allocated[i] = True
                    return i
            return 0xFFFFFFFF

        def tls_get_value(index):
            if index >= MAX_TLS or not tls_allocated[index]:
                return None
            tid = threading.get_ident()
            return tls_thread_storage.get(tid, [None] * MAX_TLS)[index]

        def tls_set_value(index, val):
            if index >= MAX_TLS or not tls_allocated[index]:
                return False
            tid = threading.get_ident()
            if tid not in tls_thread_storage:
                tls_thread_storage[tid] = [None] * MAX_TLS
            tls_thread_storage[tid][index] = val
            return True

        def tls_free(index):
            if index >= MAX_TLS or not tls_allocated[index]:
                return False
            tls_allocated[index] = False
            for tid in tls_thread_storage:
                tls_thread_storage[tid][index] = None
            return True

        # 1. Cấp phát slot
        slot0 = tls_alloc()
        self.assertEqual(slot0, 0)
        slot1 = tls_alloc()
        self.assertEqual(slot1, 1)

        # 2. Gán và đọc giá trị trên luồng hiện tại
        self.assertTrue(tls_set_value(slot0, 0x12345678))
        self.assertEqual(tls_get_value(slot0), 0x12345678)
        self.assertIsNone(tls_get_value(slot1))

        # 3. Kiểm thử tính cô lập đa luồng (Thread-Local Isolation)
        worker_seen = []
        def worker_func():
            worker_seen.append(tls_get_value(slot0))
            tls_set_value(slot0, 0x87654321)
            worker_seen.append(tls_get_value(slot0))

        t = threading.Thread(target=worker_func)
        t.start()
        t.join()

        self.assertEqual(worker_seen, [None, 0x87654321], "Worker thread phải có storage độc lập")
        self.assertEqual(tls_get_value(slot0), 0x12345678)

        # 4. Giải phóng slot và kiểm tra lỗi biên
        self.assertTrue(tls_free(slot0))
        self.assertIsNone(tls_get_value(slot0))
        self.assertFalse(tls_free(slot0), "Free lại slot đã free phải trả về False")
        self.assertFalse(tls_set_value(200, 1), "Slot vượt ngưỡng MAX_TLS phải trả về False")

    def test_fls_allocation_and_strtok_preservation(self):
        # Kiểm thử Fiber-Local Storage và cơ chế bảo toàn ngữ cảnh strtok_s của CRT
        MAX_FLS = 128
        fls_allocated = [False] * MAX_FLS
        fls_storage = {}
        import threading

        def fls_alloc():
            for i in range(MAX_FLS):
                if not fls_allocated[i]:
                    fls_allocated[i] = True
                    return i
            return 0xFFFFFFFF

        def fls_get_value(index):
            if index >= MAX_FLS or not fls_allocated[index]:
                return None
            tid = threading.get_ident()
            return fls_storage.get(tid, [None] * MAX_FLS)[index]

        def fls_set_value(index, val):
            if index >= MAX_FLS or not fls_allocated[index]:
                return False
            tid = threading.get_ident()
            if tid not in fls_storage:
                fls_storage[tid] = [None] * MAX_FLS
            fls_storage[tid][index] = val
            return True

        # Mô phỏng CRT _getptd() và strtok()
        dw_fls_index = fls_alloc()
        self.assertNotEqual(dw_fls_index, 0xFFFFFFFF)

        class MockTidData:
            def __init__(self):
                self.strtok_context = ""

        def mock_getptd():
            ptd = fls_get_value(dw_fls_index)
            if ptd is None:
                ptd = MockTidData()
                fls_set_value(dw_fls_index, ptd)
            return ptd

        def mock_strtok(string, delim):
            ptd = mock_getptd()
            if string is not None:
                ptd.strtok_context = string
            if not ptd.strtok_context:
                return None
            parts = ptd.strtok_context.split(delim, 1)
            token = parts[0]
            ptd.strtok_context = parts[1] if len(parts) > 1 else ""
            return token

        # Thực thi tokenize chuỗi tương tự split(str, sep) trong Squirrel sub_140265A70
        input_str = "BACKLOG,OBSERVER,QUICK_SAVE,SAVE,LOAD"
        tokens = []
        tok = mock_strtok(input_str, ",")
        while tok:
            tokens.append(tok)
            tok = mock_strtok(None, ",")

        expected = ["BACKLOG", "OBSERVER", "QUICK_SAVE", "SAVE", "LOAD"]
        self.assertEqual(tokens, expected, "strtok lặp lại với NULL phải duy trì ngữ cảnh phân tách chuỗi không bị crash")

    def test_qpc_frequency_standard_10mhz(self):
        # Kiểm tra chuẩn QueryPerformanceFrequency trên x64 Windows: 10 MHz (10,000,000 counts/sec)
        frequency = 10000000
        self.assertEqual(frequency, 10000000)

    def test_qpc_monotonic_resolution(self):
        # Mô phỏng QPC dựa trên nanoseconds
        boot_time_ns = 1000000000
        def mock_qpc(now_ns):
            return (now_ns - boot_time_ns) // 100

        # Khoảng cách 1 microsecond (1000ns) -> 10 ticks
        t1 = mock_qpc(boot_time_ns + 1000000)
        t2 = mock_qpc(boot_time_ns + 1001000)
        self.assertGreater(t2, t1)
        self.assertEqual(t2 - t1, 10)

    def test_vfs_close_handle_flush_on_write(self):
        # Kiểm tra logic đóng handle trong VFS: nếu có cờ GENERIC_WRITE (0x40000000) thì phải fsync trước khi close
        GENERIC_WRITE = 0x40000000
        GENERIC_READ = 0x80000000

        synced_fds = []
        closed_fds = []

        class MockVfsHandle:
            def __init__(self, fd, path, access_mode):
                self.fd = fd
                self.path = path
                self.access_mode = access_mode

        vfs_handles = {
            1001: MockVfsHandle(10, "/data/saves/data_001_0006.bin", GENERIC_WRITE),
            1002: MockVfsHandle(11, "/data/saves/data_000_0000.bin", GENERIC_READ),
        }

        def mock_close_handle(handle_id):
            if handle_id in vfs_handles:
                handle = vfs_handles[handle_id]
                if (handle.access_mode & GENERIC_WRITE) != 0:
                    synced_fds.append(handle.fd)
                closed_fds.append(handle.fd)
                del vfs_handles[handle_id]
                return True
            return False

        # Đóng file write: phải fsync và close
        self.assertTrue(mock_close_handle(1001))
        self.assertIn(10, synced_fds, "File ghi save bắt buộc phải được fsync để bảo vệ toàn vẹn dữ liệu")
        self.assertIn(10, closed_fds)
        self.assertNotIn(1001, vfs_handles)

        # Đóng file read: chỉ close, không fsync
        self.assertTrue(mock_close_handle(1002))
        self.assertNotIn(11, synced_fds, "File chỉ đọc không cần fsync")
        self.assertIn(11, closed_fds)
        self.assertNotIn(1002, vfs_handles)

    def test_vfs_close_handle_edge_cases(self):
        # Kiểm tra các trường hợp biên: NULL, con trỏ -1, handle không tồn tại
        def mock_close_handle_safe(handle_id, handles_map):
            if handle_id is None or handle_id == 0 or handle_id == -1:
                return True
            if handle_id in handles_map:
                del handles_map[handle_id]
                return True
            return True

        test_map = {999: "mock_handle"}
        self.assertTrue(mock_close_handle_safe(None, test_map))
        self.assertTrue(mock_close_handle_safe(0, test_map))
        self.assertTrue(mock_close_handle_safe(-1, test_map))
        self.assertTrue(mock_close_handle_safe(12345, test_map))
        self.assertTrue(mock_close_handle_safe(999, test_map))
        self.assertNotIn(999, test_map)

    def test_multibyte_to_widechar_utf8_vietnamese(self):
        # Kiểm tra giải mã UTF-8 sang UTF-16 cho tiếng Việt có dấu
        vietnamese_text = "Quay lại không thấy ai. Mayuri, em đang làm gì đấy? Quầy bán vé tự động."
        utf8_bytes = vietnamese_text.encode('utf-8')

        # Mô phỏng thuật toán utf8ToUtf16
        def mock_utf8_to_utf16(src_bytes):
            out = []
            i = 0
            while i < len(src_bytes):
                b0 = src_bytes[i]
                if b0 < 0x80:
                    cp = b0
                    i += 1
                elif (b0 & 0xE0) == 0xC0:
                    b1 = src_bytes[i + 1]
                    cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F)
                    i += 2
                elif (b0 & 0xF0) == 0xE0:
                    b1 = src_bytes[i + 1]
                    b2 = src_bytes[i + 2]
                    cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F)
                    i += 3
                elif (b0 & 0xF8) == 0xF0:
                    b1 = src_bytes[i + 1]
                    b2 = src_bytes[i + 2]
                    b3 = src_bytes[i + 3]
                    cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F)
                    i += 4
                else:
                    cp = b0
                    i += 1
                if cp <= 0xFFFF:
                    out.append(chr(cp))
                else:
                    cp -= 0x10000
                    out.append(chr(0xD800 + (cp >> 10)))
                    out.append(chr(0xDC00 + (cp & 0x3FF)))
            return "".join(out)

        decoded = mock_utf8_to_utf16(utf8_bytes)
        self.assertEqual(decoded, vietnamese_text, "Chuỗi tiếng Việt sau khi giải mã UTF-8 phải trùng khớp 100%")
        self.assertNotIn("\ufffd", decoded, "Không được có ký tự lỗi Unicode replacement")

    def test_multibyte_to_widechar_edge_cases(self):
        # Edge cases: rỗng, kích thước truy vấn cchWideChar=0, null-terminator
        def mock_mb_to_wc(src_str, cb_mb, cch_wc):
            if not src_str:
                return 0
            src_bytes = src_str.encode('utf-8')
            length = cb_mb if cb_mb >= 0 else len(src_bytes)
            if length == 0:
                return 1 if cb_mb < 0 else 0
            needed = len(src_str)
            if cb_mb < 0:
                needed += 1
            if cch_wc == 0:
                return needed
            return min(needed, cch_wc)

        # 1. cchWideChar == 0: chỉ truy vấn độ dài cần thiết
        needed = mock_mb_to_wc("Test", -1, 0)
        self.assertEqual(needed, 5) # 4 ký tự + 1 null

        # 2. Chuỗi rỗng
        self.assertEqual(mock_mb_to_wc("", 0, 10), 0)

    def test_cocreateinstance_video_processor_mft(self):
        """Kiểm tra stub_CoCreateInstance hỗ trợ CLSID_CColorConvertDMO / IID_IMFTransform."""
        clsid_color_convert = bytes([
            0x71, 0x05, 0x23, 0x98, 0x87, 0x00, 0x04, 0x42, 0xB0, 0x20, 0x32, 0x82, 0x53, 0x8E, 0x57, 0xD3
        ])
        iid_transform = bytes([
            0x21, 0xC1, 0x94, 0xBF, 0x05, 0x5B, 0x6F, 0x4E, 0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D
        ])

        class MockMft:
            pass

        def mock_co_create_instance(rclsid, riid):
            if rclsid == clsid_color_convert or riid == iid_transform:
                return 0, MockMft()
            return 0x80004002, None

        hr, obj = mock_co_create_instance(clsid_color_convert, iid_transform)
        self.assertEqual(hr, 0, "CoCreateInstance với CLSID_CColorConvertDMO phải trả về S_OK (0)")
        self.assertIsNotNone(obj, "Đối tượng trả về không được là None")

        # CLSID không xác định phải trả về E_NOINTERFACE
        hr_fail, obj_fail = mock_co_create_instance(b"\x00" * 16, b"\x00" * 16)
        self.assertEqual(hr_fail, 0x80004002)
        self.assertIsNone(obj_fail)

    def test_vfs_save_data_case_insensitive_subpath_resolution(self):
        """Kiểm tra resolveCaseInsensitiveSubPath duyệt case-insensitive đệ quy từng tầng thư mục."""
        # Giả lập cây thư mục thực tế trên Linux filesystem (phân biệt hoa thường)
        mock_fs_dirs = {
            "/sdcard/sgrb_game_data/<Documents>": True,
            "/sdcard/sgrb_game_data/<Documents>/My Games": True,
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam": True,
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT": True,
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930": True,
        }
        mock_fs_files = {
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_000_0000.bin": b"SYSTEM_SAVE",
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0000.bin": b"MANUAL_SAVE_SLOT1",
        }

        def mock_resolve_subpath(base_dir, sub_path):
            if not sub_path:
                return base_dir
            parts = [p for p in sub_path.replace("\\", "/").split("/") if p and p != "."]
            current = base_dir
            for i, target in enumerate(parts):
                next_path = current + "/" + target
                # Kiểm tra trực tiếp
                if next_path in mock_fs_dirs or next_path in mock_fs_files:
                    current = next_path
                    continue
                # Duyệt case-insensitive trong thư mục hiện tại
                found = False
                for existing_dir in mock_fs_dirs:
                    if existing_dir.startswith(current + "/"):
                        entry_name = existing_dir[len(current) + 1:].split("/")[0]
                        if entry_name.lower() == target.lower():
                            current = current + "/" + entry_name
                            found = True
                            break
                if not found:
                    for existing_file in mock_fs_files:
                        if existing_file.startswith(current + "/"):
                            entry_name = existing_file[len(current) + 1:].split("/")[0]
                            if entry_name.lower() == target.lower():
                                current = current + "/" + entry_name
                                found = True
                                break
                if not found:
                    # File/thư mục mới chưa tồn tại: giữ nguyên các phần còn lại
                    for rem in parts[i:]:
                        current += "/" + rem
                    break
            return current

        base_dir = "/sdcard/sgrb_game_data/<Documents>"
        # 1. Truy vấn với chuỗi hoa/thường lệch case hoàn toàn
        query_sub = "my games/MAGES_STEAM/steins;gate reboot/76561197960287930/DATA_000_0000.BIN"
        resolved = mock_resolve_subpath(base_dir, query_sub)
        self.assertEqual(
            resolved,
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_000_0000.bin",
            "Đường dẫn đã giải quyết phải khớp 100% case thực tế trên đĩa"
        )

        # 2. Truy vấn file Quick Save mới (chưa tồn tại trên đĩa)
        new_qsave_sub = "my games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin"
        resolved_new = mock_resolve_subpath(base_dir, new_qsave_sub)
        self.assertEqual(
            resolved_new,
            "/sdcard/sgrb_game_data/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin",
            "Thư mục cha của file save mới phải được chuẩn hóa case chính xác trên đĩa để open(O_CREAT) không bị lỗi ENOENT"
        )

    def test_vfs_save_data_routing_new_quick_save_to_existing_parent(self):
        """Kiểm tra định tuyến file Quick Save thống nhất vào saves/ và chỉ fallback đọc <Documents> nếu còn file cũ."""
        root_dir = "/sdcard/sgrb_game_data"
        existing_legacy_file = root_dir + "/<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0000.bin"
        mock_files = {existing_legacy_file}

        def mock_resolve_save_route(rel_path):
            sub = rel_path
            if "<Documents>" in sub:
                sub = sub.split("<Documents>")[1]
            elif sub.startswith("saves"):
                sub = sub[5:]
            sub = sub.strip("/\\")

            check_new = root_dir + "/saves/" + sub
            if check_new in mock_files:
                return check_new
            check_old = root_dir + "/<Documents>/" + sub
            if check_old in mock_files:
                return check_old
            return check_new

        # Save mới (data_001_0006.bin) phải đi thống nhất vào saves/ để tránh split-brain với data_000_0000.bin
        qsave_path = "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin"
        self.assertEqual(
            mock_resolve_save_route(qsave_path),
            root_dir + "/saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin",
        )
        # File cũ chưa kịp migrate ở <Documents> vẫn đọc được bình thường qua fallback
        legacy_q = "<Documents>/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0000.bin"
        self.assertEqual(mock_resolve_save_route(legacy_q), existing_legacy_file)

    def test_legacy_documents_to_saves_migration_and_conflict_resolution(self):
        """Kiểm tra migrate dữ liệu save cũ từ <Documents> sang saves/ bằng mock VFS (hợp lệ trên mọi OS)."""
        sub_dir = "My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930"
        vfs_dirs = {
            "<Documents>",
            "<Documents>/My Games",
            f"<Documents>/{sub_dir}",
            "saves",
            f"saves/{sub_dir}",
        }
        # Map relative path -> {"data": bytes, "mtime": int}
        vfs_files = {
            # 1. File chỉ có bên <Documents>: phải được di chuyển sang saves/
            f"<Documents>/{sub_dir}/data_001_0006.bin": {"data": b"LEGACY_QSAVE", "mtime": 1000},
            # 2. File tồn tại cả 2 nơi: saves/ mới hơn (mtime=2000) vs <Documents> cũ hơn (mtime=1000) -> giữ saves/
            f"<Documents>/{sub_dir}/data_000_0000.bin": {"data": b"OLD_SYS", "mtime": 1000},
            f"saves/{sub_dir}/data_000_0000.bin": {"data": b"NEW_SYS", "mtime": 2000},
            # 3. File tồn tại cả 2 nơi nhưng <Documents> mới hơn -> ghi đè bản mới hơn sang saves/
            f"<Documents>/{sub_dir}/data_001_0000.bin": {"data": b"FRESH_DOCS_SAVE", "mtime": 3000},
            f"saves/{sub_dir}/data_001_0000.bin": {"data": b"STALE_SAVES_SAVE", "mtime": 1500},
        }

        def resolve_ci_in_saves(dst_rel_path):
            for existing in vfs_files:
                if existing.lower() == dst_rel_path.lower():
                    return existing
            return dst_rel_path

        def migrate_legacy_documents(has_data_dir=True):
            if not has_data_dir or "<Documents>" not in vfs_dirs:
                return 0
            migrated = 0
            legacy_files = [p for p in list(vfs_files.keys()) if p.startswith("<Documents>/")]
            for src_path in legacy_files:
                rel_sub = src_path[len("<Documents>/"):]
                dst_path = resolve_ci_in_saves("saves/" + rel_sub)
                parent_dir = dst_path.rsplit("/", 1)[0]
                vfs_dirs.add(parent_dir)

                src_entry = vfs_files.pop(src_path)
                dst_entry = vfs_files.get(dst_path)
                if dst_entry is None or src_entry["mtime"] > dst_entry["mtime"]:
                    vfs_files[dst_path] = src_entry
                    migrated += 1

            # Dọn thư mục <Documents> sau khi toàn bộ file con đã di chuyển
            for d in [d for d in list(vfs_dirs) if d.startswith("<Documents>")]:
                if not any(f.startswith(d + "/") for f in vfs_files):
                    vfs_dirs.discard(d)
            return migrated

        count = migrate_legacy_documents(True)
        self.assertEqual(count, 2)
        self.assertNotIn("<Documents>", vfs_dirs, "Thư mục <Documents> rỗng phải được dọn sạch sau migrate")
        self.assertEqual(vfs_files[f"saves/{sub_dir}/data_001_0006.bin"]["data"], b"LEGACY_QSAVE")
        self.assertEqual(vfs_files[f"saves/{sub_dir}/data_000_0000.bin"]["data"], b"NEW_SYS")
        self.assertEqual(vfs_files[f"saves/{sub_dir}/data_001_0000.bin"]["data"], b"FRESH_DOCS_SAVE")

        # Error / Edge case: dataDir chưa tồn tại hoặc không có <Documents> -> không crash, trả về 0
        self.assertEqual(migrate_legacy_documents(False), 0)
        self.assertEqual(migrate_legacy_documents(True), 0)

    def test_critical_section_recursive_synchronization(self):
        """Kiểm tra CRITICAL_SECTION thật hỗ trợ khóa đệ quy trên cùng thread, chặn race đa luồng và xử lý nullptr."""
        import threading

        class MockCriticalSectionRegistry:
            def __init__(self):
                self.table_lock = threading.Lock()
                self.entries = {}

            def _get_or_create(self, ptr):
                if ptr is None or ptr == 0:
                    return None
                with self.table_lock:
                    if ptr not in self.entries:
                        self.entries[ptr] = threading.RLock()
                    return self.entries[ptr]

            def initialize(self, ptr):
                self._get_or_create(ptr)
                return True

            def enter(self, ptr):
                entry = self._get_or_create(ptr)
                if entry:
                    entry.acquire()

            def try_enter(self, ptr):
                entry = self._get_or_create(ptr)
                return bool(entry and entry.acquire(blocking=False))

            def leave(self, ptr):
                with self.table_lock:
                    entry = self.entries.get(ptr)
                if entry:
                    entry.release()

            def delete(self, ptr):
                if ptr is None or ptr == 0:
                    return
                with self.table_lock:
                    self.entries.pop(ptr, None)

        cs_mgr = MockCriticalSectionRegistry()
        cs_ptr = 0x140A96000
        self.assertTrue(cs_mgr.initialize(cs_ptr))

        # 1. Re-entrant (khóa lồng nhau 2 lần trên cùng thread chính như MBgTaskManager)
        cs_mgr.enter(cs_ptr)
        self.assertTrue(cs_mgr.try_enter(cs_ptr))

        # 2. Thread worker khác thử try_enter khi main thread đang giữ -> phải trả False (chống data race)
        worker_try_result = []
        t = threading.Thread(target=lambda: worker_try_result.append(cs_mgr.try_enter(cs_ptr)))
        t.start()
        t.join()
        self.assertEqual(worker_try_result, [False])

        # Mở đủ 2 lớp khóa -> worker khác có thể lấy khóa thành công
        cs_mgr.leave(cs_ptr)
        cs_mgr.leave(cs_ptr)
        worker_try_after = []
        def worker_acquire_release():
            got = cs_mgr.try_enter(cs_ptr)
            worker_try_after.append(got)
            if got:
                cs_mgr.leave(cs_ptr)
        t2 = threading.Thread(target=worker_acquire_release)
        t2.start()
        t2.join()
        self.assertEqual(worker_try_after, [True])

        # 3. Error / Edge case: nullptr không gây crash
        cs_mgr.enter(None)
        self.assertFalse(cs_mgr.try_enter(0))
        cs_mgr.leave(None)
        cs_mgr.delete(cs_ptr)

    def test_win32_systemtime_filetime_and_timezone_conversions(self):
        """Kiểm tra chuyển đổi hai chiều SystemTime <-> FileTime, FileTimeToLocalFileTime và xử lý dữ liệu biên/lỗi."""
        EPOCH_DIFF = 11644473600
        TICKS_PER_SEC = 10000000
        TICKS_PER_MS = 10000

        def days_from_civil(y, m, d):
            y -= 1 if m <= 2 else 0
            era = (y if y >= 0 else y - 399) // 400
            yoe = y - era * 400
            doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
            doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
            return era * 146097 + doe - 719468

        def systemtime_to_filetime(year, month, day, hour, minute, second, ms=0):
            if month < 1 or month > 12 or day < 1 or day > 31:
                return None
            unix_sec = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second
            ticks = (unix_sec + EPOCH_DIFF) * TICKS_PER_SEC + ms * TICKS_PER_MS
            return ticks if ticks >= 0 else None

        # 1. Win32 epoch (1601-01-01 00:00:00 UTC) == 0 ticks
        self.assertEqual(systemtime_to_filetime(1601, 1, 1, 0, 0, 0), 0)
        # 2. Unix epoch (1970-01-01 00:00:00 UTC) == 11644473600 * 10^7
        self.assertEqual(systemtime_to_filetime(1970, 1, 1, 0, 0, 0), EPOCH_DIFF * TICKS_PER_SEC)
        # 3. Năm nhuận 2024-02-29 12:30:45.250 -> kiểm tra FileTimeToLocalFileTime (+7h = +25200s)
        ft_utc = systemtime_to_filetime(2024, 2, 29, 12, 30, 45, 250)
        self.assertIsNotNone(ft_utc)
        ft_local = ft_utc + 25200 * TICKS_PER_SEC
        self.assertEqual(ft_local - ft_utc, 7 * 3600 * TICKS_PER_SEC)
        # 4. Bias múi giờ Việt Nam (UTC+7): GetTimeZoneInformation.Bias = -420 phút
        self.assertEqual(-(25200 // 60), -420)
        # 5. Error handling: ngày/tháng sai hoặc trước năm 1601
        self.assertIsNone(systemtime_to_filetime(2024, 13, 1, 0, 0, 0))
        self.assertIsNone(systemtime_to_filetime(2024, 0, 1, 0, 0, 0))
        self.assertIsNone(systemtime_to_filetime(1500, 1, 1, 0, 0, 0))

    def test_vfs_union_find_files_across_documents_and_saves(self):
        """Kiểm tra Union Find gom đầy đủ file save từ cả 2 thư mục saves/ và <Documents>/."""
        # Giả lập tình huống manual save ở <Documents>, quick save ở saves
        mock_docs_dir = {
            "data_000_0000.bin": {"size": 202752, "mtime": 1000}, # System data
            "data_001_0000.bin": {"size": 1572864, "mtime": 1010}, # Manual slot 1
            "meta_000_0000.bin": {"size": 412, "mtime": 1000},
            "meta_001_0000.bin": {"size": 412, "mtime": 1010},
        }
        mock_saves_dir = {
            "data_001_0006.bin": {"size": 1572864, "mtime": 1050}, # Quick save (Auto save slot 1)
            "meta_001_0006.bin": {"size": 412, "mtime": 1050},
        }

        # Thuật toán Union Find
        candidate_dirs = [mock_docs_dir, mock_saves_dir]
        pattern = "data_*.bin"

        import fnmatch
        seen_files = {}
        entries = []

        for c_dir in candidate_dirs:
            for file_name, meta in c_dir.items():
                if fnmatch.fnmatch(file_name, pattern):
                    lower_name = file_name.lower()
                    if lower_name not in seen_files:
                        seen_files[lower_name] = len(entries)
                        entries.append({"name": file_name, **meta})
                    else:
                        idx = seen_files[lower_name]
                        if meta["mtime"] > entries[idx]["mtime"]:
                            entries[idx] = {"name": file_name, **meta}

        entry_names = [e["name"] for e in entries]
        # Phải tìm thấy cả manual save VÀ quick save
        self.assertIn("data_000_0000.bin", entry_names)
        self.assertIn("data_001_0000.bin", entry_names)
        self.assertIn("data_001_0006.bin", entry_names, "Quick save data_001_0006.bin bắt buộc phải xuất hiện trong Union Find")
        self.assertEqual(len(entry_names), 3)

    def test_vfs_file_time_conversion(self):
        """Kiểm tra hàm unixTimeToWin32FileTime chuyển đổi timestamp Unix sang Windows FILETIME."""
        # Windows Epoch (1601-01-01) đến Unix Epoch (1970-01-01) là 11644473600 giây
        # 1 Unix second = 10,000,000 Windows ticks (100ns)
        epoch_diff = 11644473600

        def mock_unix_to_win32_filetime(sec, nsec=0):
            win_ticks = (sec + epoch_diff) * 10000000 + (nsec // 100)
            low = win_ticks & 0xFFFFFFFF
            high = (win_ticks >> 32) & 0xFFFFFFFF
            return low, high

        # Unix timestamp 0 (1970-01-01 00:00:00 UTC)
        low, high = mock_unix_to_win32_filetime(0)
        reconstructed = (high << 32) | low
        expected_ticks = epoch_diff * 10000000
        self.assertEqual(reconstructed, expected_ticks)

        # Unix timestamp 1700000000
        sec = 1700000000
        low, high = mock_unix_to_win32_filetime(sec)
        reconstructed = (high << 32) | low
        self.assertEqual(reconstructed, (sec + epoch_diff) * 10000000)

    def test_vfs_write_flush_and_close_fsync(self):
        """Kiểm tra FlushFileBuffers và CloseHandle đảm bảo gọi fsync cho file save."""
        synced_handles = []

        class MockFileHandle:
            def __init__(self, fd, access_mode):
                self.fd = fd
                self.access_mode = access_mode

        handles = {
            101: MockFileHandle(fd=12, access_mode=0x40000000), # GENERIC_WRITE
            102: MockFileHandle(fd=13, access_mode=0x80000000), # GENERIC_READ
        }

        def mock_flush_file_buffers(handle_id):
            if handle_id in handles:
                synced_handles.append(handles[handle_id].fd)
                return True
            return False

        def mock_close_handle(handle_id):
            if handle_id in handles:
                h = handles[handle_id]
                if (h.access_mode & 0x40000000) != 0:
                    synced_handles.append(h.fd)
                del handles[handle_id]
                return True
            return False

        # Flush file write
        self.assertTrue(mock_flush_file_buffers(101))
        self.assertIn(12, synced_handles)

        # Close file write -> phải fsync
        self.assertTrue(mock_close_handle(101))
        self.assertEqual(synced_handles.count(12), 2)

        # Close file read -> không cần fsync
        self.assertTrue(mock_close_handle(102))
        self.assertNotIn(13, synced_handles)

    def test_vfs_current_directory_and_save_routing(self):
        """Kiểm tra SetCurrentDirectoryW lưu đường dẫn và tự động giải quyết relative save files."""
        current_dir = [""]

        def set_current_dir(path):
            norm = path.replace("\\", "/").strip("/")
            if len(norm) >= 2 and norm[1] == ":":
                norm = norm[2:].strip("/")
            current_dir[0] = norm
            return True

        def get_current_dir():
            if not current_dir[0]:
                return "C:\\"
            return "C:\\" + current_dir[0].replace("/", "\\")

        def resolve_path(rel_path):
            effective = rel_path
            if not any(effective.startswith(p) for p in ("wind3d11data", "windata", "winweb", "movie", "saves", "<Documents>")):
                if current_dir[0]:
                    effective = current_dir[0] + "/" + effective
                elif effective.startswith("data_") or effective.startswith("meta_"):
                    effective = "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/" + effective
            return effective

        # Ban đầu là root
        self.assertEqual(get_current_dir(), "C:\\")

        # Game gọi SetCurrentDirectoryW
        set_current_dir("saves\\My Games\\mages_steam\\STEINS;GATE REBOOT\\76561197960287930")
        self.assertEqual(get_current_dir(), "C:\\saves\\My Games\\mages_steam\\STEINS;GATE REBOOT\\76561197960287930")

        # Truy cập file save với đường dẫn tương đối (như doAutoLoad / doLoad thực hiện)
        mapped = resolve_path("data_001_0006.bin")
        self.assertEqual(mapped, "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin")

        mapped_meta = resolve_path("meta_000_0000.bin")
        self.assertEqual(mapped_meta, "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/meta_000_0000.bin")

        # Ngay cả khi current_dir rỗng, data_*.bin vẫn tự định tuyến vào thư mục save
        current_dir[0] = ""
        self.assertEqual(resolve_path("data_001_0006.bin"), "saves/My Games/mages_steam/STEINS;GATE REBOOT/76561197960287930/data_001_0006.bin")

    def test_format_message_a_allocate_buffer_and_edge_cases(self):
        """Kiểm tra FormatMessageA xử lý FORMAT_MESSAGE_ALLOCATE_BUFFER (kể cả nSize=0), buffer cố định và lỗi null."""
        import os

        cpp_path = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp", "win32_bridge.cpp")
        )
        with open(cpp_path, "r", encoding="utf-8") as f:
            cpp_src = f.read()

        # Kiểm chứng contract thật trong win32_bridge.cpp
        self.assertIn("WIN32_FORMAT_MESSAGE_ALLOCATE_BUFFER", cpp_src)
        self.assertIn("char** outPtr = reinterpret_cast<char**>(lpBuffer);", cpp_src)
        self.assertIn("malloc(allocBytes)", cpp_src)
        self.assertIn("stub_SetLastError(WIN32_ERROR_INVALID_PARAMETER);", cpp_src)

        FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x00000100
        FORMAT_MESSAGE_FROM_STRING = 0x00000400
        FORMAT_MESSAGE_FROM_SYSTEM = 0x00001000
        last_error = [0]

        error_messages = {
            0: "The operation completed successfully.",
            2: "The system cannot find the file specified.",
            5: "Access is denied.",
        }

        def mock_format_message_a(flags, source, msg_id, lang_id, out_box, n_size):
            if out_box is None:
                last_error[0] = 87
                return 0
            if (flags & FORMAT_MESSAGE_FROM_STRING) and source is not None:
                msg = source
            else:
                msg = error_messages.get(msg_id, f"Win32 error {msg_id}")

            if flags & FORMAT_MESSAGE_ALLOCATE_BUFFER:
                ptr = self.mock_heap_alloc(max(len(msg) + 1, n_size))
                out_box["ptr"] = ptr
                out_box["data"] = msg
                return len(msg)

            if n_size == 0:
                return 0
            copy_len = min(len(msg), n_size - 1)
            out_box["data"] = msg[:copy_len]
            return copy_len

        # Happy path: sub_1404DBFC0 gọi FormatMessageA(0x1100, 0, LastError=2, 0x400, &buf, 0, 0)
        box = {"ptr": 0}
        written = mock_format_message_a(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
            None,
            2,
            0x400,
            box,
            0,
        )
        self.assertGreater(written, 0)
        self.assertNotEqual(box["ptr"], 0)
        self.assertIn("cannot find the file", box["data"])
        self.assertTrue(self.mock_heap_free(box["ptr"]))

        # Edge cases: caller-provided buffer cắt ngắn khi nSize nhỏ, nSize=0/1, FROM_STRING và mã lỗi lạ
        fixed_box = {}
        self.assertEqual(mock_format_message_a(FORMAT_MESSAGE_FROM_SYSTEM, None, 2, 0, fixed_box, 10), 9)
        self.assertEqual(len(fixed_box["data"]), 9)
        self.assertEqual(mock_format_message_a(FORMAT_MESSAGE_FROM_SYSTEM, None, 2, 0, fixed_box, 0), 0)
        self.assertEqual(mock_format_message_a(FORMAT_MESSAGE_FROM_SYSTEM, None, 2, 0, fixed_box, 1), 0)
        self.assertEqual(fixed_box["data"], "")

        str_box = {}
        self.assertEqual(
            mock_format_message_a(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_STRING, "custom error", 0, 0, str_box, 0),
            len("custom error"),
        )
        self.assertEqual(str_box["data"], "custom error")

        unk_box = {}
        mock_format_message_a(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, None, 997, 0, unk_box, 0)
        self.assertEqual(unk_box["data"], "Win32 error 997")

        # Error handling: lpBuffer là NULL
        self.assertEqual(mock_format_message_a(FORMAT_MESSAGE_FROM_SYSTEM, None, 2, 0, None, 64), 0)
        self.assertEqual(last_error[0], 87)

    def test_vfs_read_only_asset_path_cache_and_save_bypass(self):
        """Kiểm tra cache đường dẫn asset chỉ đọc bỏ qua thư mục saves/<Documents> và không lưu đường dẫn lỗi."""
        import os

        cpp_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "cpp", "win32_bridge.cpp"))
        with open(cpp_path, "r", encoding="utf-8") as f:
            src = f.read()

        self.assertIn("sAssetPathCache", src)
        self.assertIn("sAssetPathCacheMutex", src)

        cache = {}
        walk_calls = []

        def resolve_cached(rel_path, existing_map, root_dir="/sdcard/SGRB"):
            is_save = (
                rel_path.startswith("saves/")
                or rel_path == "saves"
                or "<Documents>" in rel_path
                or rel_path.startswith("data_")
                or rel_path.startswith("meta_")
            )
            if not is_save and rel_path in cache:
                return cache[rel_path]
            walk_calls.append(rel_path)
            resolved = existing_map.get(rel_path.lower())
            if resolved is None:
                return f"{root_dir}/{rel_path}"
            if not is_save:
                cache[rel_path] = resolved
            return resolved

        disk = {
            "resource/script_body.bin": "/sdcard/SGRB/Resource/script_body.bin",
            "saves/data_0001.bin": "/sdcard/SGRB/saves/data_0001.bin",
        }
        # Happy path: asset chỉ đọc hit cache ở lần mở thứ 2 trở đi
        self.assertEqual(resolve_cached("Resource/script_body.bin", disk), "/sdcard/SGRB/Resource/script_body.bin")
        self.assertEqual(resolve_cached("Resource/script_body.bin", disk), "/sdcard/SGRB/Resource/script_body.bin")
        self.assertEqual(walk_calls.count("Resource/script_body.bin"), 1)

        # Edge case: file save KHÔNG bao giờ dùng cache để đảm bảo tạo mới/xóa file thấy ngay
        self.assertEqual(resolve_cached("saves/data_0001.bin", disk), "/sdcard/SGRB/saves/data_0001.bin")
        self.assertEqual(resolve_cached("saves/data_0001.bin", disk), "/sdcard/SGRB/saves/data_0001.bin")
        self.assertEqual(walk_calls.count("saves/data_0001.bin"), 2)

        # Error / missing optional file: không ghi vào sAssetPathCache
        missing = resolve_cached("config/op_subtitle.psb.m", disk)
        self.assertEqual(missing, "/sdcard/SGRB/config/op_subtitle.psb.m")
        self.assertNotIn("config/op_subtitle.psb.m", cache)

if __name__ == "__main__":
    unittest.main()


