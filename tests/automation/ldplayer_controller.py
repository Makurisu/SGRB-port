import os
import subprocess
import time
from typing import Optional, Tuple

class LDPlayerController:
    """Điều khiển và tương tác sâu với giả lập LDPlayer qua adb và ldconsole."""

    def __init__(self, ld_dir: str = r"D:\LDPlayer\LDPlayer9"):
        self.ld_dir = ld_dir
        self.adb_path = os.path.join(ld_dir, "adb.exe")
        self.ldconsole_path = os.path.join(ld_dir, "ldconsole.exe")
        self.package_name = "com.makurisu.sgrb"
        self.main_activity = "com.makurisu.sgrb.MainActivity"

    def run_adb(self, cmd: list[str]) -> Tuple[int, str]:
        """Thực thi lệnh ADB trực tiếp."""
        full_cmd = [self.adb_path] + cmd
        proc = subprocess.run(full_cmd, capture_output=True, text=True, encoding="utf-8", errors="ignore")
        return proc.returncode, proc.stdout.strip()

    def get_screen_resolution(self) -> Tuple[int, int]:
        """Lấy độ phân giải thực của màn hình thiết bị."""
        code, out = self.run_adb(["shell", "wm", "size"])
        # Format: Physical size: 1600x900
        if "Physical size:" in out:
            parts = out.split("Physical size:")[-1].strip().split("x")
            return int(parts[0]), int(parts[1])
        return 1920, 1080

    def tap(self, x: int, y: int) -> None:
        """Giả lập chạm vào toạ độ (x, y) để chuyển thoại hoặc chọn menu."""
        self.run_adb(["shell", "input", "tap", str(x), str(y)])

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration_ms: int = 300) -> None:
        """Giả lập vuốt màn hình (Backlog, mở System Menu)."""
        self.run_adb(["shell", "input", "swipe", str(x1), str(y1), str(x2), str(y2), str(duration_ms)])

    def long_press(self, x: int, y: int, duration_ms: int = 2500) -> None:
        """Giả lập đè ngón tay để kích hoạt chế độ Skip tua nhanh."""
        self.run_adb(["shell", "input", "swipe", str(x), str(y), str(x), str(y), str(duration_ms)])

    def take_screenshot(self, output_path: str) -> bool:
        """Chụp ảnh màn hình để verify đồ hoạ E-mote và text rendering."""
        with open(output_path, "wb") as f:
            proc = subprocess.run([self.adb_path, "exec-out", "screencap", "-p"], stdout=f)
            return proc.returncode == 0

    def start_game(self) -> Tuple[int, str]:
        """Khởi động ứng dụng game."""
        return self.run_adb(["shell", "am", "start", "-n", f"{self.package_name}/{self.main_activity}"])

    def stop_game(self) -> Tuple[int, str]:
        """Dừng ứng dụng game."""
        return self.run_adb(["shell", "am", "force-stop", self.package_name])

    def read_game_logs(self, max_lines: int = 50) -> str:
        """Đọc logcat lọc riêng theo tag của game để phát hiện lỗi."""
        code, out = self.run_adb(["logcat", "-d", "-s", "SGRE_JNI:V", "SGRE_InputInjector:V", "AndroidRuntime:E"])
        lines = out.splitlines()
        return "\n".join(lines[-max_lines:])
