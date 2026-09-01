import struct
from pathlib import Path

from PyQt6.QtCore import QThread, pyqtSignal

from .constants import FRAME_SIZE, HEADER_FORMAT, dlc_to_len

def _pack_frame(timestamp_ms, node_id, can_id, dlc, payload):
    return struct.pack(
        HEADER_FORMAT,
        timestamp_ms,
        node_id,
        can_id,
        dlc,
    ) + payload[:64].ljust(64, b"\x00")


def iter_esp_binary_frames(path):
    with open(path, "rb") as log_file:
        while True:
            chunk = log_file.read(FRAME_SIZE * 1000)
            if not chunk:
                return

            usable_size = len(chunk) - (len(chunk) % FRAME_SIZE)
            for offset in range(0, usable_size, FRAME_SIZE):
                yield chunk[offset:offset + FRAME_SIZE]


def iter_savvycan_asc_frames(path):
    with open(path, "r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            fields = line.split()
            if len(fields) < 9 or fields[1] not in ("CAN", "CANFD") or fields[3] not in ("Rx", "Tx"):
                continue

            try:
                timestamp_ms = int(float(fields[0]) * 1000)
                node_id = int(fields[2])
                can_id = int(fields[4], 16)
                dlc = min(int(fields[7], 16), 15)
                byte_count = int(fields[8])
                payload_len = min(dlc_to_len(dlc), byte_count, 64)
                payload = bytes(int(value, 16) for value in fields[9:9 + payload_len])
            except ValueError:
                continue
            if len(payload) != payload_len:
                continue

            yield _pack_frame(
                timestamp_ms,
                node_id,
                can_id,
                dlc,
                payload,
            )


class LogFileLoaderThread(QThread):
    frames_loaded = pyqtSignal(list)
    load_finished = pyqtSignal(int)
    load_failed = pyqtSignal(str)

    def __init__(self, filename):
        super().__init__()
        self.filename = filename
        self.running = True

    def stop(self):
        self.running = False
        self.wait()

    def run(self):
        try:
            path = Path(self.filename)
            frame_iterator = iter_esp_binary_frames(path) if path.suffix.lower() == ".bin" else iter_savvycan_asc_frames(path)
            frame_batch = []
            frame_count = 0

            for frame in frame_iterator:
                if not self.running:
                    return
                frame_batch.append(frame)
                frame_count += 1
                if len(frame_batch) == 500:
                    self.frames_loaded.emit(frame_batch)
                    frame_batch = []

            if frame_batch:
                self.frames_loaded.emit(frame_batch)
            self.load_finished.emit(frame_count)
        except OSError as error:
            self.load_failed.emit(str(error))
        except (ValueError, struct.error) as error:
            self.load_failed.emit(f"Invalid CAN log: {error}")
