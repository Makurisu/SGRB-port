import os
import sys
import time

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8")

from ldplayer_controller import LDPlayerController

def main():
    print("[1/5] Khởi tạo kết nối LDPlayer...")
    controller = LDPlayerController(r"D:\LDPlayer\LDPlayer9")
    
    code, devices = controller.run_adb(["devices"])
    if "emulator-5554" not in devices:
        print("Lỗi: Không tìm thấy emulator-5554.")
        sys.exit(1)
    print("✓ Kết nối ADB thành công: emulator-5554")

    print("[2/5] Kiểm tra thư mục dữ liệu game trên Android...")
    data_path = "/sdcard/Android/data/com.makurisu.sgrb/files/data/"
    code, ls_out = controller.run_adb(["shell", "ls", "-lh", data_path])
    print(f"Data files:\n{ls_out}")

    print("[3/5] Giả lập chuỗi thao tác cảm ứng (Touch Pipeline Verification)...")
    width, height = controller.get_screen_resolution()
    print(f"Độ phân giải: {width}x{height}")

    # Chạm chuyển câu thoại tại giữa màn hình
    print("-> Test Action: Tap chuyển thoại tại (center)")
    controller.tap(width // 2, height // 2)
    time.sleep(0.5)

    # Vuốt mở Backlog từ dưới lên
    print("-> Test Action: Vuốt mở Backlog (Swipe Up)")
    controller.swipe(width // 2, int(height * 0.8), width // 2, int(height * 0.2), 300)
    time.sleep(0.5)

    # Vuốt mở System Menu từ trên xuống
    print("-> Test Action: Vuốt mở System Menu (Swipe Down)")
    controller.swipe(width // 2, int(height * 0.2), width // 2, int(height * 0.8), 300)
    time.sleep(0.5)

    # Giữ Skip thoại
    print("-> Test Action: Giữ 1.5s kích hoạt Skip mode (Long Press)")
    controller.long_press(width // 2, height // 2, 1500)
    time.sleep(0.5)

    print("[4/5] Chụp ảnh màn hình kiểm chứng kết quả...")
    out_img = os.path.join(os.path.dirname(__file__), "ldplayer_verification_capture.png")
    if controller.take_screenshot(out_img):
        print(f"✓ Đã chụp ảnh màn hình lưu tại: {out_img} (Size: {os.path.getsize(out_img)} bytes)")
    else:
        print("Cảnh báo: Không chụp được ảnh màn hình.")

    print("[5/5] Đọc logs kiểm tra lỗi...")
    logs = controller.read_game_logs(20)
    print("Recent logs:\n" + (logs if logs else "(Chưa có runtime error)"))

    print("\n✓ HOÀN TẤT LUỒNG TEST TỰ ĐỘNG TRÊN LDPLAYER.")

if __name__ == "__main__":
    main()
