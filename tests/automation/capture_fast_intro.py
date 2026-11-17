import os
import sys
import time
import subprocess

ADB_PATH = r"D:\LDPlayer\LDPlayer9\adb.exe"
PACKAGE_NAME = "com.makurisu.sgrb"
ACTIVITY_NAME = "com.makurisu.sgrb.MainActivity"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "fast_intro_frames")

def run_adb(cmd):
    full_cmd = [ADB_PATH] + cmd
    res = subprocess.run(full_cmd, capture_output=True, text=True, encoding="utf-8", errors="ignore")
    return res.returncode, res.stdout.strip()

def capture_screen(save_path):
    with open(save_path, "wb") as f:
        subprocess.run([ADB_PATH, "exec-out", "screencap", "-p"], stdout=f)

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print("=== BAT DAU CAPTURE TOC DO CAO CHUOI INTRO SGRB ===")

    # Dung app va don logcat
    run_adb(["shell", "am", "force-stop", PACKAGE_NAME])
    time.sleep(1)
    run_adb(["logcat", "-c"])

    # Khoi dong game
    print("Khoi dong ung dung...")
    run_adb(["shell", "am", "start", "-n", f"{PACKAGE_NAME}/{ACTIVITY_NAME}"])

    # Cho 5.5 giay dau tien de den dung thoi diem dialog Attention
    print("Cho 5.5 giay den thoi diem Dialog...")
    time.sleep(5.5)

    # Chup lien tuc 30 frames voi do tre toi thieu trong khoang 5.5s - 9.5s
    print("Bat dau chup lien tuc voi do tre toi thieu...")
    start_time = time.time()
    for idx in range(30):
        t_cur = time.time() - start_time
        frame_name = f"fast_frame_{idx:02d}_{t_cur:.2f}s.png"
        save_path = os.path.join(OUTPUT_DIR, frame_name)
        capture_screen(save_path)
        print(f"  Captured: {frame_name}")
        time.sleep(0.08)

    print("Capture hoan tat! Thu muc anh:", OUTPUT_DIR)

if __name__ == "__main__":
    main()
