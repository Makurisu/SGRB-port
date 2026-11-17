import unittest
import struct
import threading
import time

# --- Cấu trúc dữ liệu và Hằng số WinMM chuẩn Windows ---
WAVE_FORMAT_PCM = 1

MMSYSERR_NOERROR     = 0
MMSYSERR_ERROR       = 1
MMSYSERR_BADDEVICEID = 2
MMSYSERR_INVALHANDLE = 5
MMSYSERR_INVALPARAM  = 11

CALLBACK_TYPEMASK    = 0x00070000
CALLBACK_NULL        = 0x00000000
CALLBACK_WINDOW      = 0x00010000
CALLBACK_TASK        = 0x00020000
CALLBACK_FUNCTION    = 0x00030000
CALLBACK_EVENT       = 0x00050000

WHDR_DONE            = 0x00000001
WHDR_PREPARED        = 0x00000002
WHDR_BEGINLOOP       = 0x00000004
WHDR_ENDLOOP         = 0x00000008
WHDR_INQUEUE         = 0x00000010

WOM_OPEN             = 0x3BB
WOM_CLOSE            = 0x3BC
WOM_DONE             = 0x3BD

TIME_MS              = 0x0001
TIME_SAMPLES         = 0x0002
TIME_BYTES           = 0x0004

WAVE_FORMAT_1M08     = 0x00000001
WAVE_FORMAT_1S08     = 0x00000002
WAVE_FORMAT_1M16     = 0x00000004
WAVE_FORMAT_1S16     = 0x00000008
WAVE_FORMAT_2M08     = 0x00000010
WAVE_FORMAT_2S08     = 0x00000020
WAVE_FORMAT_2M16     = 0x00000040
WAVE_FORMAT_2S16     = 0x00000080
WAVE_FORMAT_4M08     = 0x00000100
WAVE_FORMAT_4S08     = 0x00000200
WAVE_FORMAT_4M16     = 0x00000400
WAVE_FORMAT_4S16     = 0x00000800
WAVE_FORMAT_44S16    = 0x00000800
WAVE_FORMAT_48M08    = 0x00001000
WAVE_FORMAT_48S08    = 0x00002000
WAVE_FORMAT_48M16    = 0x00004000
WAVE_FORMAT_48S16    = 0x00008000


class MockWaveHdr:
    def __init__(self, data: bytes):
        self.lpData = data
        self.dwBufferLength = len(data)
        self.dwBytesRecorded = 0
        self.dwUser = 0
        self.dwFlags = 0
        self.dwLoops = 0
        self.lpNext = None
        self.reserved = 0


class MockWaveFormatEx:
    def __init__(self, sample_rate=48000, channels=2, bits_per_sample=16):
        self.wFormatTag = WAVE_FORMAT_PCM
        self.nChannels = channels
        self.nSamplesPerSec = sample_rate
        self.wBitsPerSample = bits_per_sample
        self.nBlockAlign = channels * (bits_per_sample // 8)
        self.nAvgBytesPerSec = sample_rate * self.nBlockAlign
        self.cbSize = 0


class MockAudioSink:
    """Mô phỏng kiến trúc và logic luồng phát âm thanh của AudioSink C++."""
    def __init__(self):
        self.is_open = False
        self.is_running = False
        self.is_paused = False
        self.is_resetting = False
        self.sample_rate = 48000
        self.channels = 2
        self.bits_per_sample = 16
        self.bytes_per_frame = 4
        self.avg_bytes_per_sec = 192000
        self.dw_callback = None
        self.callback_type = CALLBACK_NULL
        self.total_frames_played = 0
        self.queue = []
        self.lock = threading.Lock()
        self.event_signaled = False

    def get_num_devs(self):
        return 1

    def get_dev_caps(self):
        return {
            "wMid": 1,
            "wPid": 1,
            "vDriverVersion": 0x0100,
            "szPname": "Android AAudio Output",
            "dwFormats": 0x000FFFFF,
            "wChannels": 2,
            "dwSupport": 0x0003,
        }

    def open(self, fmt: MockWaveFormatEx, dw_callback=None, fdw_open=0):
        with self.lock:
            if self.is_open:
                self.close()

            if fmt:
                self.sample_rate = fmt.nSamplesPerSec
                self.channels = fmt.nChannels
                self.bits_per_sample = fmt.wBitsPerSample
                self.bytes_per_frame = self.channels * (self.bits_per_sample // 8)
                self.avg_bytes_per_sec = self.sample_rate * self.bytes_per_frame

            self.dw_callback = dw_callback
            self.callback_type = fdw_open & CALLBACK_TYPEMASK
            self.is_open = True
            self.is_running = True
            self.is_paused = False
            self.is_resetting = False
            self.total_frames_played = 0
            self.queue.clear()
            self.event_signaled = False
            return MMSYSERR_NOERROR, 0x8001

    def close(self):
        with self.lock:
            if not self.is_open:
                return MMSYSERR_NOERROR
            self.is_running = False
            # Dọn dẹp hàng đợi
            for hdr in self.queue:
                hdr.dwFlags &= ~WHDR_INQUEUE
                hdr.dwFlags |= WHDR_DONE
                self._notify_callback(hdr)
            self.queue.clear()
            self.is_open = False
            return MMSYSERR_NOERROR

    def write(self, hdr: MockWaveHdr):
        if not hdr or not hdr.lpData or hdr.dwBufferLength == 0:
            return MMSYSERR_INVALPARAM

        with self.lock:
            hdr.dwFlags |= WHDR_INQUEUE
            hdr.dwFlags &= ~WHDR_DONE
            self.queue.append(hdr)
            return MMSYSERR_NOERROR

    def process_one_buffer(self):
        """Mô phỏng 1 chu kỳ xử lý của luồng worker."""
        hdr = None
        with self.lock:
            if not self.is_running or self.is_paused or not self.queue:
                return None
            hdr = self.queue.pop(0)

        frames = hdr.dwBufferLength // self.bytes_per_frame
        with self.lock:
            self.total_frames_played += frames
            hdr.dwFlags &= ~WHDR_INQUEUE
            hdr.dwFlags |= WHDR_DONE
            self._notify_callback(hdr)
        return hdr

    def reset(self):
        with self.lock:
            self.is_resetting = True
            for hdr in self.queue:
                hdr.dwFlags &= ~WHDR_INQUEUE
                hdr.dwFlags |= WHDR_DONE
                self._notify_callback(hdr)
            self.queue.clear()
            self.is_resetting = False
            return MMSYSERR_NOERROR

    def pause(self):
        with self.lock:
            self.is_paused = True
            return MMSYSERR_NOERROR

    def restart(self):
        with self.lock:
            self.is_paused = False
            return MMSYSERR_NOERROR

    def get_position(self, time_type=TIME_BYTES):
        with self.lock:
            bytes_played = self.total_frames_played * self.bytes_per_frame
            if time_type == TIME_MS:
                ms = (bytes_played * 1000) // self.avg_bytes_per_sec if self.avg_bytes_per_sec > 0 else 0
                return MMSYSERR_NOERROR, ms
            elif time_type == TIME_SAMPLES:
                return MMSYSERR_NOERROR, self.total_frames_played
            else:
                return MMSYSERR_NOERROR, bytes_played

    def _notify_callback(self, hdr: MockWaveHdr):
        if self.callback_type == CALLBACK_EVENT:
            self.event_signaled = True
            if callable(self.dw_callback):
                self.dw_callback()


class TestAudioSinkSubsystem(unittest.TestCase):
    def setUp(self):
        self.sink = MockAudioSink()

    def test_wave_out_get_num_devs(self):
        """Kiểm tra số lượng thiết bị đầu ra trả về 1."""
        self.assertEqual(self.sink.get_num_devs(), 1)

    def test_wave_out_get_dev_caps_formats(self):
        """Kiểm tra cờ định dạng âm thanh đáp ứng kiểm tra format miniaudio (0x1400CF6D0)."""
        caps = self.sink.get_dev_caps()
        dw_formats = caps["dwFormats"]

        # miniaudio kiểm tra bit 0x8000 (WAVE_FORMAT_48S16) và 0x800 (WAVE_FORMAT_44S16)
        self.assertTrue((dw_formats & WAVE_FORMAT_48S16) != 0, "Thiếu định dạng 48kHz 16-bit Stereo!")
        self.assertTrue((dw_formats & WAVE_FORMAT_44S16) != 0, "Thiếu định dạng 44.1kHz 16-bit Stereo!")
        self.assertEqual(caps["wChannels"], 2, "Kênh âm thanh phải là 2 (Stereo)!")
        self.assertEqual(caps["szPname"], "Android AAudio Output")

    def test_miniaudio_format_negotiation_simulation(self):
        """Giả lập thuật toán chọn định dạng sub_1400CF6D0 của miniaudio."""
        caps = self.sink.get_dev_caps()
        dw_formats = caps["dwFormats"]
        channels = caps["wChannels"]

        # Thuật toán trong sub_1400CF6D0:
        # if a2 == 2 (stereo):
        #   if (a1 & 0x8000) -> 48000Hz, 16-bit
        #   elif (a1 & 0x800) -> 44100Hz, 16-bit
        selected_rate = None
        selected_bits = None
        if channels == 2:
            if (dw_formats & 0x8000) != 0:
                selected_rate = 48000
                selected_bits = 16
            elif (dw_formats & 0x800) != 0:
                selected_rate = 44100
                selected_bits = 16

        self.assertEqual(selected_rate, 48000, "miniaudio phải tự động chọn 48000Hz!")
        self.assertEqual(selected_bits, 16, "miniaudio phải tự động chọn 16-bit PCM!")

    def test_wave_out_open_and_close(self):
        """Kiểm tra mở và đóng thiết bị WaveOut."""
        fmt = MockWaveFormatEx(48000, 2, 16)
        status, hwo = self.sink.open(fmt, dw_callback=None, fdw_open=CALLBACK_NULL)
        self.assertEqual(status, MMSYSERR_NOERROR)
        self.assertIsNotNone(hwo)
        self.assertTrue(self.sink.is_open)

        close_status = self.sink.close()
        self.assertEqual(close_status, MMSYSERR_NOERROR)
        self.assertFalse(self.sink.is_open)

    def test_wave_out_write_flag_transitions(self):
        """Kiểm tra chuyển trạng thái WHDR_INQUEUE và WHDR_DONE khi gửi và hoàn tất buffer."""
        fmt = MockWaveFormatEx(48000, 2, 16)
        self.sink.open(fmt, dw_callback=None, fdw_open=CALLBACK_EVENT)

        # Buffer 20ms PCM stereo 16-bit: 960 frames * 4 bytes = 3840 bytes
        pcm_bytes = b'\x00' * 3840
        hdr = MockWaveHdr(pcm_bytes)

        # Gửi buffer vào hàng đợi
        status = self.sink.write(hdr)
        self.assertEqual(status, MMSYSERR_NOERROR)
        self.assertTrue((hdr.dwFlags & WHDR_INQUEUE) != 0, "Buffer phải được bật WHDR_INQUEUE!")
        self.assertEqual((hdr.dwFlags & WHDR_DONE), 0, "WHDR_DONE phải bị xóa khi đang xếp hàng!")

        # Worker xử lý phát buffer
        processed = self.sink.process_one_buffer()
        self.assertIs(processed, hdr)
        self.assertEqual((hdr.dwFlags & WHDR_INQUEUE), 0, "WHDR_INQUEUE phải bị xóa sau khi phát xong!")
        self.assertTrue((hdr.dwFlags & WHDR_DONE) != 0, "WHDR_DONE phải được bật sau khi phát xong!")
        self.assertTrue(self.sink.event_signaled, "Event callback phải được SetEvent để miniaudio không deadlock!")

    def test_wave_out_callback_event_prevents_miniaudio_deadlock(self):
        """Kiểm tra luồng sự kiện SetEvent đánh thức miniaudio đúng cách."""
        fmt = MockWaveFormatEx(48000, 2, 16)
        callback_fired = []

        def on_event():
            callback_fired.append(True)

        self.sink.open(fmt, dw_callback=on_event, fdw_open=CALLBACK_EVENT)

        hdr1 = MockWaveHdr(b'\x00' * 1920)
        hdr2 = MockWaveHdr(b'\x00' * 1920)

        self.sink.write(hdr1)
        self.sink.write(hdr2)

        self.assertEqual(len(callback_fired), 0)

        # Phát buffer 1
        self.sink.process_one_buffer()
        self.assertEqual(len(callback_fired), 1, "Buffer 1 phát xong phải kích hoạt SetEvent ngay!")
        self.assertTrue((hdr1.dwFlags & WHDR_DONE) != 0)

        # Phát buffer 2
        self.sink.process_one_buffer()
        self.assertEqual(len(callback_fired), 2, "Buffer 2 phát xong phải kích hoạt SetEvent ngay!")
        self.assertTrue((hdr2.dwFlags & WHDR_DONE) != 0)

    def test_wave_out_reset_drains_queue(self):
        """Kiểm tra waveOutReset dọn sạch hàng đợi và hoàn tất toàn bộ buffer dở dang."""
        fmt = MockWaveFormatEx(48000, 2, 16)
        self.sink.open(fmt, dw_callback=None, fdw_open=CALLBACK_EVENT)

        hdr1 = MockWaveHdr(b'\x00' * 1000)
        hdr2 = MockWaveHdr(b'\x00' * 1000)
        self.sink.write(hdr1)
        self.sink.write(hdr2)
        self.assertEqual(len(self.sink.queue), 2)

        # Gọi reset
        self.sink.reset()
        self.assertEqual(len(self.sink.queue), 0, "Hàng đợi phải được dọn sạch!")
        self.assertTrue((hdr1.dwFlags & WHDR_DONE) != 0, "Buffer 1 phải được đánh dấu WHDR_DONE khi reset!")
        self.assertTrue((hdr2.dwFlags & WHDR_DONE) != 0, "Buffer 2 phải được đánh dấu WHDR_DONE khi reset!")

    def test_wave_out_pause_and_restart(self):
        """Kiểm tra tạm dừng và tiếp tục luồng âm thanh."""
        fmt = MockWaveFormatEx(48000, 2, 16)
        self.sink.open(fmt, dw_callback=None, fdw_open=CALLBACK_EVENT)

        self.sink.pause()
        self.assertTrue(self.sink.is_paused)

        hdr = MockWaveHdr(b'\x00' * 1000)
        self.sink.write(hdr)

        # Khi pause, process_one_buffer không được tiêu thụ buffer
        result = self.sink.process_one_buffer()
        self.assertIsNone(result)

        self.sink.restart()
        self.assertFalse(self.sink.is_paused)

        # Sau khi restart, buffer được phát bình thường
        result = self.sink.process_one_buffer()
        self.assertIs(result, hdr)

    def test_wave_out_get_position(self):
        """Kiểm tra tính toán vị trí phát theo bytes, samples và ms."""
        fmt = MockWaveFormatEx(48000, 2, 16) # 4 bytes / frame, 192000 bytes / sec
        self.sink.open(fmt, dw_callback=None, fdw_open=CALLBACK_EVENT)

        # Phát 48000 frames (1 giây) = 192000 bytes
        hdr = MockWaveHdr(b'\x00' * 192000)
        self.sink.write(hdr)
        self.sink.process_one_buffer()

        _, bytes_pos = self.sink.get_position(TIME_BYTES)
        self.assertEqual(bytes_pos, 192000)

        _, samples_pos = self.sink.get_position(TIME_SAMPLES)
        self.assertEqual(samples_pos, 48000)

        _, ms_pos = self.sink.get_position(TIME_MS)
        self.assertEqual(ms_pos, 1000)

    def test_invalid_params_rejected(self):
        """Kiểm tra xử lý lỗi khi tham số không hợp lệ."""
        status = self.sink.write(None)
        self.assertEqual(status, MMSYSERR_INVALPARAM)

        empty_hdr = MockWaveHdr(b'')
        status2 = self.sink.write(empty_hdr)
        self.assertEqual(status2, MMSYSERR_INVALPARAM)


if __name__ == "__main__":
    unittest.main()
