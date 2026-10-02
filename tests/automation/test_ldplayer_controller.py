import os
import unittest
from ldplayer_controller import LDPlayerController

class TestLDPlayerController(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.controller = LDPlayerController(r"D:\LDPlayer\LDPlayer9")

    def test_adb_connection(self):
        code, out = self.controller.run_adb(["devices"])
        self.assertEqual(code, 0)
        self.assertIn("emulator-5554", out)

    def test_screen_resolution(self):
        width, height = self.controller.get_screen_resolution()
        self.assertGreater(width, 0)
        self.assertGreater(height, 0)
        self.assertEqual((width, height), (1600, 900))

    def test_touch_simulation(self):
        # Kiểm tra giả lập chạm không gây lỗi ADB
        width, height = self.controller.get_screen_resolution()
        centerX, centerY = width // 2, height // 2
        self.controller.tap(centerX, centerY)

    def test_screenshot_capture(self):
        test_shot = os.path.join(os.path.dirname(__file__), "test_screen.png")
        success = self.controller.take_screenshot(test_shot)
        self.assertTrue(success)
        self.assertTrue(os.path.exists(test_shot))
        self.assertGreater(os.path.getsize(test_shot), 1000)
        # Dọn dẹp file sau khi test theo quy tắc
        if os.path.exists(test_shot):
            os.remove(test_shot)

if __name__ == "__main__":
    unittest.main()
