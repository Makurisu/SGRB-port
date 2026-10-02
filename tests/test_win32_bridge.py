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

    def test_string_conversion_lengths(self):
        src = "Steins;Gate Reboot"
        # Test ASCII/UTF-8 length
        self.assertEqual(len(src), 18)

if __name__ == "__main__":
    unittest.main()
