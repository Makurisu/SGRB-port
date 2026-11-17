import os
import sys
import time
import subprocess
from PIL import Image
import numpy as np

ADB_PATH = r"D:\LDPlayer\LDPlayer9\adb.exe"
PACKAGE_NAME = "com.makurisu.sgrb"
ACTIVITY_NAME = "com.makurisu.sgrb.MainActivity"
OUT_DIR = r"D:\Code\STEINS.GATE.REBOOT\STEINS.GATE.REBOOT\SGRB-port\tests\automation\run_logo_transitions"

def run_adb(cmd):
    full_cmd = [ADB_PATH] + cmd
    res = subprocess.run(full_cmd, capture_output=True, text=True, encoding="utf-8", errors="ignore")
    return res.returncode, res.stdout.strip()

def capture_screen(save_path):
    with open(save_path, "wb") as f:
        subprocess.run([ADB_PATH, "exec-out", "screencap", "-p"], stdout=f)

def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    print("=== DUNG APP VA KHOI DONG LAI ===")
    run_adb(["shell", "am", "force-stop", PACKAGE_NAME])
    time.sleep(1)
    run_adb(["logcat", "-c"])

    print("=== KHOI DONG GAME ===")
    run_adb(["shell", "am", "start", "-n", f"{PACKAGE_NAME}/{ACTIVITY_NAME}"])
    start_time = time.time()

    # Chụp liên tục từ giây 0.5s đến 9.0s để quan sát toàn bộ chuỗi chuyển cảnh logo
    frames = []
    idx = 0
    print("=== CHUP CHUOI FRAME CHUYEN CANH LOGO (0.5s -> 9.0s) ===")
    while time.time() - start_time < 9.0:
        elapsed = time.time() - start_time
        path = os.path.join(OUT_DIR, f"logo_{idx:02d}_{elapsed:.2f}s.png")
        capture_screen(path)
        frames.append((elapsed, path))
        idx += 1
        time.sleep(0.15)

    print(f"Da chup {len(frames)} frames.")

    # Lấy logcat
    logcat_path = os.path.join(OUT_DIR, "logcat_logo.txt")
    with open(logcat_path, "w", encoding="utf-8") as f:
        subprocess.run([ADB_PATH, "logcat", "-d", "-s", "SGRE_D3D11Bridge", "SGRE_JNI"], stdout=f)
    print("Da luu logcat.")

    # Phân tích độ sáng từng frame để vẽ đường cong chuyển tiếp (Fade Curve)
    print("\n=== DUONG CONG CHUYEN CANH (FADE TRANSITION CURVE) ===")
    brightness_list = []
    for elapsed, fpath in frames:
        if os.path.exists(fpath):
            img = np.array(Image.open(fpath))
            mean_rgb = img.mean(axis=(0, 1))[:3]
            bright = float(mean_rgb.mean())
            brightness_list.append((elapsed, bright))
            # Hiển thị thanh đo độ sáng trực quan
            bar = "#" * int(bright / 2)
            print(f"[{elapsed:5.2f}s] Brightness: {bright:6.2f} | {bar}")

    # Kiểm tra xem có sự chuyển tiếp mượt mà hay không:
    # Nếu mượt mà: độ sáng sẽ có các giá trị trung gian (không nhảy vọt từ 0 lên >30 trong 1 frame)
    intermediate_frames = [b for t, b in brightness_list if 1.0 < b < 25.0]
    print(f"\nSo khung hinh fade trung gian (1.0 < Brightness < 25.0): {len(intermediate_frames)}")
    if len(intermediate_frames) >= 4:
        print("XAC NHAN: Hoat anh fade transition dien ra rat muot ma voi nhieu cap do do sang trung gian!")
    else:
        print("CHU Y: It cap do do sang trung gian.")

if __name__ == "__main__":
    main()
