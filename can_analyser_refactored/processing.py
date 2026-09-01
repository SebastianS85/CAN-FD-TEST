import struct
import time
import threading
from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from .constants import FRAME_SIZE, HEADER_FORMAT, HEADER_SIZE, QThread, dlc_to_len, pyqtSignal, struct, threading, time
class DataProcessorThread(QThread):
    frames_ready = pyqtSignal(list)

    def __init__(self, incoming_buffer):
        super().__init__()
        self.incoming_buffer = incoming_buffer
        self.running = True
        self.is_paused = False
        self.filter_text = ""
        self.selected_bus_idx = 0
        self.is_delta_mode = False
        self.chart_targets = []
        self.sniffer_target_id = -1
        self.db = None
        
        self.stats_lock = threading.Lock()
        self.total_frames = 0
        self.bus1_count, self.bus2_count = 0, 0
        self.frames_in_last_sec, self.bytes_in_last_sec = 0, 0
        self.id_statistics = {}
        self.id_last_timestamp = {}
        self.id_frequencies = {}
        self.max_esp_timestamp = 0
        self.last_global_timestamp = 0
        
        self.chart_buffers = [[] for _ in range(4)]
        self.latest_sniffed_data = None

    def update_settings(self, is_paused, filter_text, bus_idx, is_delta, chart_targets, sniffer_id):
        self.is_paused = is_paused
        self.filter_text = filter_text
        self.selected_bus_idx = bus_idx
        self.is_delta_mode = is_delta
        self.chart_targets = chart_targets
        self.sniffer_target_id = sniffer_id

    def update_db(self, db):
        self.db = db

    def clear_stats(self):
        with self.stats_lock:
            self.total_frames = 0
            self.bus1_count, self.bus2_count = 0, 0
            self.frames_in_last_sec, self.bytes_in_last_sec = 0, 0
            self.id_statistics.clear()
            self.id_last_timestamp.clear()
            self.id_frequencies.clear()
            self.max_esp_timestamp = 0
            self.last_global_timestamp = 0
            self.chart_buffers = [[] for _ in range(4)]
            self.latest_sniffed_data = None

    def get_and_reset_speed(self):
        with self.stats_lock:
            f, b = self.frames_in_last_sec, self.bytes_in_last_sec
            self.frames_in_last_sec, self.bytes_in_last_sec = 0, 0
            return f, b

    def get_and_clear_chart_buffers(self):
        with self.stats_lock:
            res = self.chart_buffers
            self.chart_buffers = [[] for _ in range(4)]
            return res

    def run(self):
        accumulated_gui_frames = []
        last_gui_emit = time.perf_counter()

        while self.running:
            if self.is_paused:
                time.sleep(0.05)
                continue

            batch = []
            while self.incoming_buffer:
                batch.append(self.incoming_buffer.popleft())

            if batch:
                processed_frames = []
                local_frames, local_bytes = 0, 0
                
                with self.stats_lock:
                    for data in batch:
                        if len(data) != FRAME_SIZE: 
                            continue
                        
                        try:
                            timestamp, node_id, can_id, dlc = struct.unpack_from(HEADER_FORMAT, data, 0)
                            actual_len = dlc_to_len(dlc)
                            if actual_len < 0 or actual_len > 64: actual_len = 8
                                
                            raw_data = data[10 : 10 + actual_len]
                            self.total_frames += 1
                            local_frames += 1
                            local_bytes += (HEADER_SIZE + actual_len)
                            
                            if timestamp > self.max_esp_timestamp:
                                self.max_esp_timestamp = timestamp

                            clean_id = can_id & 0x1FFFFFFF 
                            
                            if clean_id == self.sniffer_target_id:
                                self.latest_sniffed_data = raw_data[:actual_len]
                            
                            if clean_id in self.id_last_timestamp:
                                dt = (timestamp - self.id_last_timestamp[clean_id]) / 1000.0
                                if dt > 0:
                                    inst_hz = 1.0 / dt
                                    old_hz = self.id_frequencies.get(clean_id, inst_hz)
                                    self.id_frequencies[clean_id] = 0.8 * old_hz + 0.2 * inst_hz
                            
                            self.id_last_timestamp[clean_id] = timestamp
                            self.id_statistics[clean_id] = self.id_statistics.get(clean_id, 0) + 1

                            if node_id == 1: self.bus1_count += 1
                            elif node_id == 2: self.bus2_count += 1

                            for idx, (t_id, t_byte) in enumerate(self.chart_targets):
                                if t_id != -1 and clean_id == t_id and 0 <= t_byte < actual_len:
                                    self.chart_buffers[idx].append((timestamp / 1000.0, raw_data[t_byte]))

                            if self.selected_bus_idx == 1 and node_id != 1: continue
                            if self.selected_bus_idx == 2 and node_id != 2: continue

                            if self.filter_text:
                                matched = False
                                try:
                                    if "-" in self.filter_text:
                                        p = self.filter_text.split("-")
                                        if int(p[0].strip(), 16) <= clean_id <= int(p[1].strip(), 16): matched = True
                                    else:
                                        if clean_id == int(self.filter_text.replace("0x", ""), 16): matched = True
                                except ValueError: pass
                                if not matched: continue

                            if self.is_delta_mode:
                                if self.last_global_timestamp == 0: time_val_str = "0 ms"
                                else:
                                    delta_ms = timestamp - self.last_global_timestamp
                                    time_val_str = f"+{delta_ms} ms" if delta_ms >= 0 else f"{delta_ms} ms"
                                self.last_global_timestamp = timestamp
                            else:
                                time_val_str = f"{timestamp} ms"

                            hex_str = " ".join(f"{b:02X}" for b in raw_data)
                            id_str = f"0x{clean_id:03X}" if clean_id <= 0x7FF else f"0x{clean_id:08X} (Ext)"
                            
                            dbc_str = "-"
                            if self.db and CANTOOLS_AVAILABLE:
                                try:
                                    decoded = self.db.decode_message(clean_id, bytes(raw_data))
                                    dbc_str = ", ".join([f"{k}={v}" for k, v in decoded.items()])
                                except Exception:
                                    pass

                            processed_frames.append({
                                'no': self.total_frames, 'timestamp': timestamp, 'time': time_val_str, 'node_id': node_id,
                                'id_str': id_str, 'len': actual_len, 'hex_str': hex_str,
                                'dbc_str': dbc_str, 'clean_id': clean_id, 'raw_data': raw_data
                            })
                            
                        except struct.error:
                            continue
                        except Exception:
                            continue
                        
                    self.frames_in_last_sec += local_frames
                    self.bytes_in_last_sec += local_bytes

                if processed_frames:
                    accumulated_gui_frames.extend(processed_frames)

            now = time.perf_counter()
            if len(accumulated_gui_frames) >= 200 or (now - last_gui_emit) >= 0.05:
                if accumulated_gui_frames:
                    self.frames_ready.emit(accumulated_gui_frames)
                    accumulated_gui_frames = []
                last_gui_emit = now

            if not batch:
                time.sleep(0.005)

    def stop(self):
        self.running = False
        self.wait()

