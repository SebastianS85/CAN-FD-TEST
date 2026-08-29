import socket
import struct
import sys
import os
import time
import logging
import threading
import csv
import configparser
from collections import deque
from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from PyQt6.QtGui import QColor, QFont, QPainter, QRegularExpressionValidator
from PyQt6.QtWidgets import (
    QApplication, QHeaderView, QLabel, QTableView,
    QMainWindow, QPushButton, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget, QHBoxLayout, 
    QCheckBox, QLineEdit, QComboBox, QMessageBox, QTabWidget,
    QFileDialog, QGroupBox, QFormLayout, QScrollArea, QFrame
)

# --- LOGGING CONFIGURATION ---
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler("can_python_errors.log", encoding='utf-8'),
        logging.StreamHandler(sys.stdout)
    ]
)

try:
    import cantools
    CANTOOLS_AVAILABLE = True
except ImportError:
    CANTOOLS_AVAILABLE = False
    logging.warning("Cantools library not found - DBC decoding is disabled.")

try:
    from PyQt6.QtCharts import QChart, QChartView, QLineSeries, QValueAxis
    CHARTS_AVAILABLE = True
except ImportError:
    CHARTS_AVAILABLE = False
    logging.warning("PyQt6-Charts library not found - charts are disabled.")

# --- PERSISTENCE CONFIGURATION (CONFIG.INI) ---
CONFIG_FILE = "config.ini"

def load_config():
    config = configparser.ConfigParser()
    if os.path.exists(CONFIG_FILE):
        config.read(CONFIG_FILE)
    
    ip = config.get("Network", "ip", fallback="192.168.178.45")
    port = config.getint("Network", "port", fallback=3333)
    return ip, port

def save_config(ip, port):
    config = configparser.ConfigParser()
    config["Network"] = {
        "ip": ip,
        "port": str(port)
    }
    with open(CONFIG_FILE, "w") as f:
        config.write(f)

TCP_IP, TCP_PORT = load_config()

HEADER_FORMAT = "<IBIB"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
FRAME_SIZE = 74  # Strict size of log_frame_t from ESP32

# --- UI TRANSLATIONS ---
TRANSLATIONS = {
    "PL": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "channel": "Kanał:", "all_channels": "Wszystkie",
        "filter_id": "Filtr ID:", "filter_ph": "np. 123", "speed": "Prędkość:",
        "pause": "Pauza", "resume": "Wznów", "autoscroll": "Auto-scroll", "delta_time": "Delta (Δt)", 
        "clear": "Wyczyść", "export_csv": "💾 CSV", "ip_label": "IP:",
        "headers": ["Lp.", "Czas / Delta", "Magistrala", "CAN ID (Hex)", "DLC", "Dane Payload (Hex)", "Sygnały DBC"],
        "stats_headers": ["CAN ID (Hex)", "Liczba ramek", "Częstotliwość (Hz)", "Ostatni Czas", "Status"],
        "tab_monitor": "Monitor Surowy + DBC", "tab_plots": "Wykresy ID", "tab_stats": "Statystyki", "tab_gen": "Generator TX", "tab_sniffer": "Bit Sniffer",
        "stats_summary": "Ogółem: {} | CAN 1: {} | CAN 2: {} | Unikalnych ID: {} | DBC: {}",
        "form_bus": "Kanał docelowy:", "form_id": "CAN ID (Hex):", "form_ext": "Extended ID (29-bit):", "form_data": "Dane Hex:", "form_interval": "Send Every (ms):",
        "btn_send_once": "Wyślij raz", "btn_start_cyclic": "Start Cykliczny", "btn_stop_cyclic": "Stop Cykliczny",
        "plot_id": "ID", "plot_byte": "Bajt:", "sniffer_id": "Śledzone ID (Hex):",
        "msg_err": "Błąd", "msg_succ": "Sukces", "msg_net_err": "Błąd Sieci",
        "msg_no_cantools": "Brak biblioteki cantools!", "msg_loaded": "Załadowano: ",
        "msg_bad_interval": "Niepoprawny interwał!", "msg_build_err": "Nie można zbudować ramki: ",
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Wyeksportowano do: "
    },
    "EN": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "channel": "Channel:", "all_channels": "All",
        "filter_id": "ID Filter:", "filter_ph": "e.g. 123", "speed": "Speed:",
        "pause": "Pause", "resume": "Resume", "autoscroll": "Auto-scroll", "delta_time": "Delta (Δt)", 
        "clear": "Clear", "export_csv": "💾 CSV", "ip_label": "IP:",
        "headers": ["No.", "Time / Delta", "Bus", "CAN ID (Hex)", "DLC", "Payload Data (Hex)", "DBC Signals"],
        "stats_headers": ["CAN ID (Hex)", "Count", "Frequency (Hz)", "Last Timestamp", "Status"],
        "tab_monitor": "Raw Monitor + DBC", "tab_plots": "ID Charts", "tab_stats": "Statistics", "tab_gen": "TX Generator", "tab_sniffer": "Bit Sniffer",
        "stats_summary": "Total: {} | CAN 1: {} | CAN 2: {} | Unique IDs: {} | DBC: {}",
        "form_bus": "Target Channel:", "form_id": "CAN ID (Hex):", "form_ext": "Extended ID (29-bit):", "form_data": "Data (Hex):", "form_interval": "Send Every (ms):",
        "btn_send_once": "Send Once", "btn_start_cyclic": "Start Cyclic", "btn_stop_cyclic": "Stop Cyclic",
        "plot_id": "ID", "plot_byte": "Byte:", "sniffer_id": "Tracked ID (Hex):",
        "msg_err": "Error", "msg_succ": "Success", "msg_net_err": "Network Error",
        "msg_no_cantools": "Cantools library is missing!", "msg_loaded": "Loaded: ",
        "msg_bad_interval": "Invalid interval!", "msg_build_err": "Cannot build frame: ",
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Exported to: "
    },
    "DE": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "channel": "Kanal:", "all_channels": "Alle",
        "filter_id": "ID-Filter:", "filter_ph": "z.B. 123", "speed": "Geschwindigkeit:",
        "pause": "Pause", "resume": "Fortsetzen", "autoscroll": "Auto-Scroll", "delta_time": "Delta (Δt)", 
        "clear": "Löschen", "export_csv": "💾 CSV", "ip_label": "IP:",
        "headers": ["Nr.", "Zeit / Delta", "Bus", "CAN ID (Hex)", "DLC", "Nutzdaten (Hex)", "DBC Signale"],
        "stats_headers": ["CAN ID (Hex)", "Anzahl", "Frequenz (Hz)", "Letzter Zeitst.", "Status"],
        "tab_monitor": "Rohmonitor + DBC", "tab_plots": "ID Diagramme", "tab_stats": "Statistik", "tab_gen": "TX Generator", "tab_sniffer": "Bit Sniffer",
        "stats_summary": "Gesamt: {} | CAN 1: {} | CAN 2: {} | IDs: {} | DBC: {}",
        "form_bus": "Zielkanal:", "form_id": "CAN ID (Hex):", "form_ext": "Extended ID (29-bit):", "form_data": "Daten (Hex):", "form_interval": "Senden alle (ms):",
        "btn_send_once": "Einmal senden", "btn_start_cyclic": "Zyklisch Starten", "btn_stop_cyclic": "Zyklisch Stoppen",
        "plot_id": "ID", "plot_byte": "Byte:", "sniffer_id": "Verfolgte ID (Hex):",
        "msg_err": "Fehler", "msg_succ": "Erfolg", "msg_net_err": "Netzwerkfehler",
        "msg_no_cantools": "Cantools-Bibliothek fehlt!", "msg_loaded": "Geladen: ",
        "msg_bad_interval": "Ungültiges Intervall!", "msg_build_err": "Frame kann nicht erstellt werden: ",
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Exportiert nach: "
    }
}

INDUSTRIAL_STYLESHEET = """
QMainWindow { background-color: #1b1b1b; color: #cccccc; font-family: "Segoe UI", sans-serif; }
QPushButton { background-color: #2c2c2c; color: #e0e0e0; border: 1px solid #3c3c3c; border-radius: 4px; padding: 6px 10px; font-weight: bold; font-size: 11px; }
QPushButton:hover { background-color: #383838; border: 1px solid #505050; }
QPushButton:pressed { background-color: #222222; }
QPushButton:checked { background-color: #d28e00; color: #111; border: 1px solid #b87a00; }
QPushButton#btnSendOnce { background-color: #114b7d; border: 1px solid #1f6feb; }
QPushButton#btnSendOnce:hover { background-color: #1f6feb; }
QPushButton#btnStartCyclic { background-color: #1b5e20; border: 1px solid #238636; }
QPushButton#btnStartCyclic:hover { background-color: #238636; }
QPushButton#btnStopCyclic { background-color: #7a1c1c; border: 1px solid #da3633; }
QPushButton#btnStopCyclic:hover { background-color: #f85149; }
QPushButton#btnExport { background-color: #d28e00; color: #111; border: 1px solid #b87a00; }
QPushButton#btnExport:hover { background-color: #e5a417; }
QPushButton:disabled { background-color: #181818; color: #555555; border: 1px solid #242424; }
QLineEdit, QComboBox { background-color: #141414; color: #00dd99; border: 1px solid #333333; border-radius: 3px; padding: 4px; }
QTabWidget::pane { border: 1px solid #333333; background-color: #161616; }
QTabBar::tab { background-color: #222222; color: #999999; padding: 6px 14px; border-top-left-radius: 3px; border-top-right-radius: 3px; margin-right: 2px; }
QTabBar::tab:selected { background-color: #2c2c2c; color: #ffffff; font-weight: bold; }
QGroupBox { border: 1px solid #383838; border-radius: 4px; margin-top: 8px; font-weight: bold; color: #aaaaaa; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
QTableView { background-color: #141414; color: #00e5ff; gridline-color: #242424; outline: none; border: none; }
QTableView::item:selected { background-color: #2c3e50; }
QHeaderView::section { background-color: #222222; color: #cccccc; padding: 4px; border: 1px solid #333; }
QScrollArea { border: none; background-color: transparent; }

/* Chart Specific Styles */
QFrame#chartControlBar { background-color: #222222; border: 1px solid #333333; border-radius: 6px; }
QLineEdit.chartInput { background-color: #121212; color: #ffffff; border: 1px solid #444; font-weight: bold; }
QLineEdit.chartInput:focus { border: 1px solid #6ec5ff; background-color: #1a1a1a; }
"""

def dlc_to_len(dlc):
    if dlc <= 8: return dlc
    mapping = {9: 12, 10: 16, 11: 20, 12: 24, 13: 32, 14: 48, 15: 64}
    return mapping.get(dlc, 8)

def len_to_dlc(length):
    if length <= 8: return length, length
    elif length <= 12: return 9, 12
    elif length <= 16: return 10, 16
    elif length <= 20: return 11, 20
    elif length <= 24: return 12, 24
    elif length <= 32: return 13, 32
    elif length <= 48: return 14, 48
    else: return 15, 64

class BitGridWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumSize(400, 2000)
        self.current_data = bytearray()
        self.max_bytes = 64
        self.fade_levels = [[0.0 for _ in range(8)] for _ in range(self.max_bytes)]
        self.fade_colors = [[(0,0,0) for _ in range(8)] for _ in range(self.max_bytes)]
        self.color_0 = QColor(40, 40, 40)       
        self.color_1 = QColor(200, 200, 200)    
        self.color_up = QColor(255, 50, 50)     
        self.color_down = QColor(50, 150, 255)  
        self.fade_timer = QTimer(self)
        self.fade_timer.timeout.connect(self.decay_colors)
        self.fade_timer.start(50)

    def update_data(self, new_data):
        if not new_data: return
        if len(self.current_data) != len(new_data):
            self.current_data = bytearray(new_data)
            self.fade_levels = [[0.0]*8 for _ in range(self.max_bytes)]
            self.update()
            return

        changed = False
        for byte_idx in range(len(new_data)):
            old_b = self.current_data[byte_idx]
            new_b = new_data[byte_idx]
            if old_b != new_b:
                changed = True
                for bit_idx in range(8):
                    old_bit = (old_b >> (7 - bit_idx)) & 1
                    new_bit = (new_b >> (7 - bit_idx)) & 1
                    if old_bit != new_bit:
                        self.fade_levels[byte_idx][bit_idx] = 1.0
                        self.fade_colors[byte_idx][bit_idx] = self.color_up if new_bit else self.color_down

        self.current_data = bytearray(new_data)
        if changed: self.update()

    def decay_colors(self):
        if not self.isVisible(): return
        needs_update = False
        for r in range(len(self.current_data)):
            for c in range(8):
                if self.fade_levels[r][c] > 0.0:
                    self.fade_levels[r][c] -= 0.05
                    if self.fade_levels[r][c] <= 0.0: self.fade_levels[r][c] = 0.0
                    needs_update = True
        if needs_update: self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        cell_w, cell_h, margin_x, margin_y = 30, 30, 40, 20
        painter.setFont(QFont("Consolas", 10, QFont.Weight.Bold))
        
        for byte_idx in range(len(self.current_data)):
            painter.setPen(QColor(150, 150, 150))
            painter.drawText(5, margin_y + byte_idx * cell_h + 20, f"B{byte_idx:02}:")
            val = self.current_data[byte_idx]
            painter.setPen(QColor(0, 221, 153))
            painter.drawText(margin_x + 8*cell_w + 15, margin_y + byte_idx * cell_h + 20, f"{val:02X}")
            
            for bit_idx in range(8):
                bit_val = (val >> (7 - bit_idx)) & 1
                fade = self.fade_levels[byte_idx][bit_idx]
                base_color = self.color_1 if bit_val else self.color_0
                if fade > 0:
                    target_color = self.fade_colors[byte_idx][bit_idx]
                    r = int(base_color.red() + (target_color.red() - base_color.red()) * fade)
                    g = int(base_color.green() + (target_color.green() - base_color.green()) * fade)
                    b = int(base_color.blue() + (target_color.blue() - base_color.blue()) * fade)
                    paint_color = QColor(r, g, b)
                else:
                    paint_color = base_color
                    
                rect_x, rect_y = margin_x + bit_idx * cell_w, margin_y + byte_idx * cell_h
                painter.setBrush(paint_color)
                painter.setPen(QColor(30, 30, 30))
                painter.drawRect(rect_x, rect_y, cell_w - 2, cell_h - 2)
                painter.setPen(QColor(0, 0, 0) if bit_val else QColor(150, 150, 150))
                painter.drawText(rect_x + 10, rect_y + 19, str(bit_val))

class TCPReceiverThread(QThread):
    connection_status = pyqtSignal(bool, str)

    def __init__(self, ip, port, buffer_deque):
        super().__init__()
        self.ip, self.port = ip, port
        self.running = True
        self.buffer_deque = buffer_deque
        self.rx_socket = None
        self.tx_socket = None

    def update_ip(self, new_ip):
        if self.ip != new_ip:
            self.ip = new_ip
            self.close_sockets()

    def close_sockets(self):
        if self.rx_socket:
            try: self.rx_socket.close()
            except: pass
            self.rx_socket = None
        if self.tx_socket:
            try: self.tx_socket.close()
            except: pass
            self.tx_socket = None

    def run(self):
        logging.info(f"TCP client thread started. Target: {self.ip}:{self.port}")

        while self.running:
            self.connection_status.emit(False, "SEARCHING...")
            try:
                # --- ODBIÓR DANYCH Z ESP32 (Port 3333) ---
                sock_rx = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_rx.settimeout(2.0)
                sock_rx.connect((self.ip, self.port))
                
                # --- WYSYŁANIE KOMEND DO ESP32 (Port 3334) ---
                sock_tx = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_tx.settimeout(2.0)
                sock_tx.connect((self.ip, self.port + 1))
                
                sock_tx.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                
                self.rx_socket = sock_rx
                self.tx_socket = sock_tx
                
                self.connection_status.emit(True, "CONNECTED")
                logging.info(f"Connected to ESP32 at {self.ip} (RX: {self.port}, TX: {self.port+1})")

                stream_buffer = bytearray()
                
                while self.running:
                    try:
                        chunk = self.rx_socket.recv(1024 * 64) 
                        if not chunk: 
                            logging.warning("ESP32 closed connection.")
                            break 
                        stream_buffer.extend(chunk)
                        
                        while len(stream_buffer) >= FRAME_SIZE:
                            frame_data = stream_buffer[:FRAME_SIZE]
                            self.buffer_deque.append(frame_data)
                            del stream_buffer[:FRAME_SIZE]
                            
                    except socket.timeout:
                        continue
                    except Exception as e:
                        logging.error(f"TCP stream error: {e}")
                        break
                        
                self.close_sockets()
                logging.warning("Disconnected from ESP32. Retrying...")
                
            except (socket.timeout, ConnectionRefusedError, OSError):
                self.close_sockets()
                time.sleep(1.0)
                continue
            except Exception as e:
                self.close_sockets()
                logging.error(f"TCP Error: {e}")
                time.sleep(1.0)

    def send_tcp_data(self, payload):
        if self.tx_socket:
            try:
                self.tx_socket.sendall(payload)
                return True
            except Exception as e:
                logging.error(f"TX error: {e}")
        return False

    def stop(self):
        self.running = False
        self.close_sockets()
        self.wait()

class CyclicSenderThread(QThread):
    def __init__(self, tcp_thread, payload, interval_ms):
        super().__init__()
        self.tcp_thread = tcp_thread
        self.payload = payload
        self.interval_sec = interval_ms / 1000.0
        self.running = True

    def run(self):
        while self.running:
            start_time = time.perf_counter()
            self.tcp_thread.send_tcp_data(self.payload)
            sleep_duration = self.interval_sec - (time.perf_counter() - start_time)
            if sleep_duration > 0: time.sleep(sleep_duration)

    def stop(self):
        self.running = False
        self.wait()

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
                                'no': self.total_frames, 'time': time_val_str, 'node_id': node_id,
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

class CANTableModel(QAbstractTableModel):
    def __init__(self, headers):
        super().__init__()
        self.headers = headers
        self.frames = []
        self.MAX_FRAMES = 5000  

    def rowCount(self, parent=QModelIndex()): return len(self.frames)
    def columnCount(self, parent=QModelIndex()): return len(self.headers)

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return self.headers[section]
        return None

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid(): return None
        frame = self.frames[index.row()]

        if role == Qt.ItemDataRole.DisplayRole:
            col = index.column()
            if col == 0: return str(frame['no'])
            elif col == 1: return frame['time']
            elif col == 2: return f"CAN {frame['node_id']}"
            elif col == 3: return frame['id_str']
            elif col == 4: return str(frame['len'])
            elif col == 5: return frame['hex_str']
            elif col == 6: return frame['dbc_str']

        elif role == Qt.ItemDataRole.BackgroundRole:
            return QColor(20, 20, 20) if frame['node_id'] == 1 else QColor(24, 24, 28)
        elif role == Qt.ItemDataRole.ForegroundRole:
            return QColor(0, 229, 255) if frame['node_id'] == 1 else QColor(100, 221, 235)
        elif role == Qt.ItemDataRole.TextAlignmentRole:
            return Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        return None

    def add_frames(self, new_frames):
        if not new_frames: return
        if len(self.frames) + len(new_frames) > self.MAX_FRAMES:
            cut_amount = len(new_frames) + 1000
            self.beginRemoveRows(QModelIndex(), 0, cut_amount - 1)
            del self.frames[:cut_amount]
            self.endRemoveRows()
        self.beginInsertRows(QModelIndex(), len(self.frames), len(self.frames) + len(new_frames) - 1)
        self.frames.extend(new_frames)
        self.endInsertRows()

    def clear_data(self):
        self.beginResetModel()
        self.frames.clear()
        self.endResetModel()

    def update_headers(self, headers):
        self.headers = headers
        self.headerDataChanged.emit(Qt.Orientation.Horizontal, 0, len(self.headers) - 1)

class CANViewerFullWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.current_lang = "PL"
        self.incoming_buffer = deque()
        self.cyclic_sender_thread = None
        self.db = None
        self.dbc_filename = "-"

        self.chart_points = [deque() for _ in range(4)]
        self.chart_start_t = None

        self.setup_ui()

        self.tcp_thread = TCPReceiverThread(TCP_IP, TCP_PORT, self.incoming_buffer)
        self.tcp_thread.connection_status.connect(self.update_connection_status)
        self.tcp_thread.start()

        self.processor_thread = DataProcessorThread(self.incoming_buffer)
        self.processor_thread.frames_ready.connect(self.on_frames_ready)
        self.processor_thread.start()

        self.retranslate_ui()

        self.filter_input.textChanged.connect(self.send_settings_to_thread)
        self.bus_filter_combo.currentIndexChanged.connect(self.send_settings_to_thread)
        self.delta_cb.stateChanged.connect(self.send_settings_to_thread)
        self.sniffer_id_input.textChanged.connect(self.send_settings_to_thread)

        self.speed_timer = QTimer()
        self.speed_timer.timeout.connect(self.update_periodic_timers)
        self.speed_timer.start(1000)

        self.scroll_timer = QTimer()
        self.scroll_timer.timeout.connect(self.check_autoscroll)
        self.scroll_timer.start(100)

        self.fast_ui_timer = QTimer()
        self.fast_ui_timer.timeout.connect(self.update_fast_ui)
        self.fast_ui_timer.start(65)

        self.send_settings_to_thread()

    def get_t(self, key):
        return TRANSLATIONS[self.current_lang].get(key, key)

    def setup_ui(self):
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        self.resize(1500, 850)
        main_layout = QVBoxLayout(central_widget)

        top_layout = QHBoxLayout()
        self.lang_combo = QComboBox()
        self.lang_combo.addItems(["Polski (PL)", "English (EN)", "Deutsch (DE)"])
        self.lang_combo.currentIndexChanged.connect(self.change_language)
        
        self.status_label = QLabel("DISCONNECTED")
        self.status_label.setFixedWidth(100)
        self.status_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.status_label.setStyleSheet("background-color: #b71c1c; color: #ffffff; font-weight: bold; padding: 4px; border-radius: 4px;")
        
        self.ip_label = QLabel("IP:")
        self.ip_label.setStyleSheet("color: #7bdcff; font-weight: bold; padding: 0 4px;")

        self.ip_input = QLineEdit(TCP_IP)
        self.ip_input.setPlaceholderText("192.168.1.x")
        self.ip_input.setFixedWidth(110)
        self.ip_input.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.ip_input.setStyleSheet(
            "QLineEdit {"
            "background-color: #0f1720;"
            "color: #80ecff;"
            "border: 1px solid #2d6cdf;"
            "border-radius: 4px;"
            "padding: 3px 6px;"
            "}"
            "QLineEdit:focus { border: 1px solid #6ec5ff; }"
        )
        self.ip_input.editingFinished.connect(self.save_and_reconnect_ip)
        
        self.load_dbc_btn = QPushButton()
        self.load_dbc_btn.clicked.connect(self.load_dbc_file)
        
        self.channel_label = QLabel()
        self.bus_filter_combo = QComboBox()
        self.filter_label = QLabel()
        self.filter_input = QLineEdit()
        self.filter_input.setMaximumWidth(80)
        
        self.speed_label = QLabel()
        self.speed_label.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
        self.speed_label.setStyleSheet("color: #ffb74d;")
        
        self.pause_btn = QPushButton()
        self.pause_btn.setCheckable(True)
        self.pause_btn.setMinimumWidth(80)
        self.pause_btn.clicked.connect(self.toggle_pause)
        
        self.delta_cb = QCheckBox()
        self.autoscroll_cb = QCheckBox()
        self.autoscroll_cb.setChecked(True)
        
        self.export_btn = QPushButton()
        self.export_btn.setObjectName("btnExport")
        self.export_btn.clicked.connect(self.export_csv)

        self.clear_btn = QPushButton()
        self.clear_btn.clicked.connect(self.clear_table)
        
        top_layout.addWidget(self.lang_combo)
        top_layout.addWidget(self.status_label)
        top_layout.addWidget(self.ip_label)
        top_layout.addWidget(self.ip_input)
        top_layout.addSpacing(10)
        top_layout.addWidget(self.load_dbc_btn)
        top_layout.addWidget(self.channel_label)
        top_layout.addWidget(self.bus_filter_combo)
        top_layout.addWidget(self.filter_label)
        top_layout.addWidget(self.filter_input)
        top_layout.addStretch()
        top_layout.addWidget(self.speed_label)
        top_layout.addWidget(self.pause_btn)
        top_layout.addWidget(self.delta_cb)
        top_layout.addWidget(self.autoscroll_cb)
        top_layout.addWidget(self.export_btn)
        top_layout.addWidget(self.clear_btn)
        main_layout.addLayout(top_layout)

        self.tabs = QTabWidget()
        main_layout.addWidget(self.tabs)

        monitor_tab = QWidget()
        monitor_layout = QVBoxLayout(monitor_tab)
        
        self.table = QTableView()
        self.table_model = CANTableModel([])
        self.table.setModel(self.table_model)
        self.table.verticalHeader().setVisible(False) 
        self.table.setAlternatingRowColors(True)
        self.table.setShowGrid(False)
        self.table.setColumnWidth(0, 80)
        self.table.setColumnWidth(1, 100)
        self.table.setColumnWidth(2, 60)
        self.table.setColumnWidth(3, 100)
        self.table.setColumnWidth(4, 40)
        self.table.setColumnWidth(5, 200)
        self.table.horizontalHeader().setSectionResizeMode(6, QHeaderView.ResizeMode.Stretch)
        monitor_layout.addWidget(self.table)
        self.tabs.addTab(monitor_tab, "")

        plot_tab = QWidget()
        plot_layout = QVBoxLayout(plot_tab)
        if CHARTS_AVAILABLE:
            plot_control_bar = QFrame()
            plot_control_bar.setObjectName("chartControlBar")
            plot_ctrl = QHBoxLayout(plot_control_bar)
            plot_ctrl.setContentsMargins(15, 10, 15, 10)
            plot_ctrl.setSpacing(25)

            self.chart_inputs = []
            self.chart_labels_id = []
            self.chart_labels_b = []
            
            colors = [QColor(255, 80, 80), QColor(80, 255, 80), QColor(80, 180, 255), QColor(255, 200, 80)]
            self.series_list = []
            
            self.chart = QChart()
            self.chart.setTheme(QChart.ChartTheme.ChartThemeDark)
            self.chart.setBackgroundVisible(False)
            
            hex_validator = QRegularExpressionValidator(QRegularExpression("[0-9A-Fa-f]{1,8}"))
            byte_validator = QRegularExpressionValidator(QRegularExpression("^[0-9]$|^[1-5][0-9]$|^6[0-3]$"))

            for i in range(4):
                group = QWidget()
                glayout = QHBoxLayout(group)
                glayout.setContentsMargins(0, 0, 0, 0)
                glayout.setSpacing(6)
                
                lbl_id = QLabel()
                lbl_id.setStyleSheet(f"color: {colors[i].name()}; font-weight: bold; font-size: 12px;")
                
                inp_id = QLineEdit()
                inp_id.setProperty("class", "chartInput")
                inp_id.setValidator(hex_validator)
                inp_id.setAlignment(Qt.AlignmentFlag.AlignCenter)
                inp_id.setMaximumWidth(65)
                inp_id.setPlaceholderText("Hex")
                
                lbl_b = QLabel()
                lbl_b.setStyleSheet("color: #aaaaaa; font-size: 11px;")
                
                inp_b = QLineEdit()
                inp_b.setProperty("class", "chartInput")
                inp_b.setValidator(byte_validator)
                inp_b.setAlignment(Qt.AlignmentFlag.AlignCenter)
                inp_b.setMaximumWidth(40)
                inp_b.setPlaceholderText("0-63")
                
                if i == 0:
                    inp_id.setText("321")
                    inp_b.setText("0")
                
                glayout.addWidget(lbl_id)
                glayout.addWidget(inp_id)
                glayout.addSpacing(5)
                glayout.addWidget(lbl_b)
                glayout.addWidget(inp_b)
                
                self.chart_labels_id.append(lbl_id)
                self.chart_labels_b.append(lbl_b)
                self.chart_inputs.append((inp_id, inp_b))
                plot_ctrl.addWidget(group)
                
                series = QLineSeries()
                series.setName(f"Trace {i+1}")
                series.setColor(colors[i])
                
                pen = series.pen()
                pen.setWidth(2)
                series.setPen(pen)
                
                self.chart.addSeries(series)
                self.series_list.append(series)
                
                inp_id.textChanged.connect(self.send_settings_to_thread)
                inp_b.textChanged.connect(self.send_settings_to_thread)

            plot_ctrl.addStretch()
            plot_layout.addWidget(plot_control_bar)

            self.axis_x = QValueAxis()
            self.axis_x.setLabelFormat("%.1f")
            self.axis_x.setRange(0, 10)
            self.chart.addAxis(self.axis_x, Qt.AlignmentFlag.AlignBottom)

            self.axis_y = QValueAxis()
            self.axis_y.setRange(0, 255)
            self.chart.addAxis(self.axis_y, Qt.AlignmentFlag.AlignLeft)
            
            for series in self.series_list:
                series.attachAxis(self.axis_x)
                series.attachAxis(self.axis_y)

            self.chart_view = QChartView(self.chart)
            self.chart_view.setRenderHint(QPainter.RenderHint.Antialiasing)
            plot_layout.addWidget(self.chart_view)
        else:
            plot_layout.addWidget(QLabel("PyQt6-Charts not available"))
        self.tabs.addTab(plot_tab, "")

        stats_tab = QWidget()
        stats_layout = QVBoxLayout(stats_tab)
        self.stats_table = QTableWidget()
        self.stats_table.setColumnCount(5)
        self.stats_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.stats_table.setStyleSheet("QTableWidget { background-color: #141414; color: #ffb74d; gridline-color: #242424; } QHeaderView::section { background-color: #222222; color: #cccccc; padding: 4px; border: none; }")
        stats_layout.addWidget(self.stats_table)
        self.tabs.addTab(stats_tab, "")

        gen_tab = QWidget()
        gen_layout = QVBoxLayout(gen_tab)
        self.form_group = QGroupBox()
        self.form_layout = QFormLayout()
        
        self.tx_bus_combo = QComboBox()
        self.tx_bus_combo.addItems(["CAN 1", "CAN 2"])
        self.tx_id_input = QLineEdit("0x321")
        self.tx_ext_id_cb = QCheckBox()
        self.tx_data_input = QLineEdit("00 11 22 33")
        self.tx_interval_input = QLineEdit("300")
        self.tx_interval_input.setMaximumWidth(100)
        
        interval_layout = QHBoxLayout()
        interval_layout.addWidget(self.tx_interval_input)
        self.interval_unit_label = QLabel("ms")
        interval_layout.addWidget(self.interval_unit_label)
        interval_layout.addStretch()

        self.lbl_bus = QLabel()
        self.lbl_id = QLabel()
        self.lbl_ext = QLabel()
        self.lbl_data = QLabel()
        self.lbl_interval = QLabel()

        self.form_layout.addRow(self.lbl_bus, self.tx_bus_combo)
        self.form_layout.addRow(self.lbl_id, self.tx_id_input)
        self.form_layout.addRow(self.lbl_ext, self.tx_ext_id_cb)
        self.form_layout.addRow(self.lbl_data, self.tx_data_input)
        self.form_layout.addRow(self.lbl_interval, interval_layout)
        
        btn_layout = QHBoxLayout()
        self.send_once_btn = QPushButton()
        self.send_once_btn.setObjectName("btnSendOnce")
        self.send_once_btn.clicked.connect(lambda: self.send_can_frame_tcp(is_cyclic=False))
        self.start_cyclic_btn = QPushButton()
        self.start_cyclic_btn.setObjectName("btnStartCyclic")
        self.start_cyclic_btn.clicked.connect(self.start_cyclic_transmission)
        self.stop_cyclic_btn = QPushButton()
        self.stop_cyclic_btn.setObjectName("btnStopCyclic")
        self.stop_cyclic_btn.clicked.connect(self.stop_cyclic_transmission)
        self.stop_cyclic_btn.setEnabled(False)

        btn_layout.addWidget(self.send_once_btn)
        btn_layout.addWidget(self.start_cyclic_btn)
        btn_layout.addWidget(self.stop_cyclic_btn)
        self.form_layout.addRow("", btn_layout)
        self.form_group.setLayout(self.form_layout)
        gen_layout.addWidget(self.form_group)
        gen_layout.addStretch()
        self.tabs.addTab(gen_tab, "")

        sniffer_tab = QWidget()
        sniffer_layout = QVBoxLayout(sniffer_tab)
        sniffer_ctrl = QHBoxLayout()
        self.sniffer_id_label = QLabel()
        sniffer_ctrl.addWidget(self.sniffer_id_label)
        self.sniffer_id_input = QLineEdit("0x123")
        self.sniffer_id_input.setMaximumWidth(80)
        sniffer_ctrl.addWidget(self.sniffer_id_input)
        sniffer_ctrl.addStretch()
        sniffer_layout.addLayout(sniffer_ctrl)

        self.scroll_area = QScrollArea()
        self.bit_grid = BitGridWidget()
        self.scroll_area.setWidget(self.bit_grid)
        self.scroll_area.setWidgetResizable(True)
        sniffer_layout.addWidget(self.scroll_area)
        self.tabs.addTab(sniffer_tab, "")

        self.stats_label = QLabel()
        main_layout.addWidget(self.stats_label)

    def save_and_reconnect_ip(self):
        new_ip = self.ip_input.text().strip()
        if new_ip and new_ip != self.tcp_thread.ip:
            save_config(new_ip, TCP_PORT)
            self.tcp_thread.update_ip(new_ip)
            logging.info(f"New IP saved. Auto-connecting to {new_ip}")

    def update_connection_status(self, is_connected, message):
        self.status_label.setText(message)
        if is_connected:
            self.status_label.setStyleSheet("background-color: #1b5e20; color: #ffffff; font-weight: bold; padding: 4px; border-radius: 4px;")
            self.ip_input.setStyleSheet("background-color: #0f1720; color: #80ecff; border: 1px solid #4caf50; border-radius: 4px; padding: 3px 6px;")
        else:
            self.status_label.setStyleSheet("background-color: #b71c1c; color: #ffffff; font-weight: bold; padding: 4px; border-radius: 4px;")
            self.ip_input.setStyleSheet("background-color: #2b0e0e; color: #ff8080; border: 1px solid #f44336; border-radius: 4px; padding: 3px 6px;")

    def send_settings_to_thread(self):
        filter_text = self.filter_input.text().strip().lower()
        bus_idx = self.bus_filter_combo.currentIndex()
        is_delta = self.delta_cb.isChecked()
        is_paused = self.pause_btn.isChecked()
        
        chart_targets = []
        if CHARTS_AVAILABLE:
            for inp_id, inp_b in self.chart_inputs:
                try:
                    c_id = int(inp_id.text().strip(), 16)
                    c_b = int(inp_b.text().strip())
                    chart_targets.append((c_id, c_b))
                except ValueError:
                    chart_targets.append((-1, -1))
        else:
            chart_targets = [(-1, -1)] * 4
                
        sniffer_id = -1
        try:
            sniffer_id = int(self.sniffer_id_input.text().strip().replace("0x", ""), 16)
        except ValueError:
            pass

        self.processor_thread.update_settings(is_paused, filter_text, bus_idx, is_delta, chart_targets, sniffer_id)

    def on_frames_ready(self, processed_frames):
        self.table_model.add_frames(processed_frames)

    def check_autoscroll(self):
        if self.autoscroll_cb.isChecked() and self.tabs.currentIndex() == 0:
            self.table.scrollToBottom()

    def export_csv(self):
        if not self.table_model.frames: return
        filename, _ = QFileDialog.getSaveFileName(self, self.get_t("export_csv"), "can_log.csv", "CSV Files (*.csv)")
        if filename:
            try:
                with open(filename, 'w', newline='', encoding='utf-8') as f:
                    writer = csv.writer(f)
                    writer.writerow(["No", "Time", "Node_ID", "CAN_ID_Hex", "DLC", "Data_Hex", "Decoded"])
                    for fr in self.table_model.frames:
                        writer.writerow([fr['no'], fr['time'], fr['node_id'], f"{fr['clean_id']:X}", fr['len'], fr['hex_str'], fr['dbc_str']])
                QMessageBox.information(self, self.get_t("msg_succ"), f"{self.get_t('export_succ')}{filename}")
            except Exception as e:
                QMessageBox.critical(self, self.get_t("msg_err"), str(e))

    def update_periodic_timers(self):
        f_sec, b_sec = self.processor_thread.get_and_reset_speed()
        self.speed_label.setText(f"{self.get_t('speed')} {f_sec} r/s | {b_sec / 1024:.1f} kB/s")

        with self.processor_thread.stats_lock:
            total = self.processor_thread.total_frames
            bus1 = self.processor_thread.bus1_count
            bus2 = self.processor_thread.bus2_count
            unique = len(self.processor_thread.id_statistics)
            
        self.stats_label.setText(self.get_t("stats_summary").format(total, bus1, bus2, unique, self.dbc_filename))

        if self.tabs.currentIndex() == 2:
            self.stats_table.setRowCount(0)
            with self.processor_thread.stats_lock:
                max_esp_ts = self.processor_thread.max_esp_timestamp
                id_stats_copy = self.processor_thread.id_statistics.copy()
                id_freqs_copy = self.processor_thread.id_frequencies.copy()
                id_lasts_copy = self.processor_thread.id_last_timestamp.copy()

            for can_id, count in sorted(id_stats_copy.items()):
                row = self.stats_table.rowCount()
                self.stats_table.insertRow(row)
                hz = id_freqs_copy.get(can_id, 0.0)
                last_t = id_lasts_copy.get(can_id, 0)
                
                item_status = QTableWidgetItem(self.get_t("status_ok"))
                item_status.setForeground(QColor(76, 175, 80))
                if (max_esp_ts - last_t) > 1000:
                    item_status.setText(self.get_t("status_timeout"))
                    item_status.setForeground(QColor(244, 67, 54))

                self.stats_table.setItem(row, 0, QTableWidgetItem(f"0x{can_id:03X}"))
                self.stats_table.setItem(row, 1, QTableWidgetItem(str(count)))
                self.stats_table.setItem(row, 2, QTableWidgetItem(f"{hz:.1f} Hz"))
                self.stats_table.setItem(row, 3, QTableWidgetItem(f"{last_t} ms"))
                self.stats_table.setItem(row, 4, item_status)

    def update_fast_ui(self):
        if self.pause_btn.isChecked(): return
        
        if CHARTS_AVAILABLE:
            buffers = self.processor_thread.get_and_clear_chart_buffers()
            
            if self.tabs.currentIndex() == 1:
                max_t = None
                for i, buf in enumerate(buffers):
                    if buf:
                        pts = self.chart_points[i]
                        for t_sec, val in buf:
                            pts.append(QPointF(t_sec, val))
                            if self.chart_start_t is None:
                                self.chart_start_t = t_sec
                        
                        if max_t is None or pts[-1].x() > max_t:
                            max_t = pts[-1].x()
                
                if max_t is not None:
                    for i in range(4):
                        pts = self.chart_points[i]
                        while len(pts) > 0 and pts[0].x() < max_t - 10.0:
                            pts.popleft()
                        
                        while len(pts) > 5000:
                            pts.popleft()
                            
                        if len(pts) > 0:
                            self.series_list[i].replace(list(pts))
                    
                    start_x = self.chart_start_t if (self.chart_start_t is not None and (max_t - self.chart_start_t) < 10.0) else max_t - 10.0
                    self.axis_x.setRange(start_x, max_t + 0.5)
            else:
                for i, buf in enumerate(buffers):
                    if buf:
                        pts = self.chart_points[i]
                        for t_sec, val in buf:
                            pts.append(QPointF(t_sec, val))
                            if self.chart_start_t is None:
                                self.chart_start_t = t_sec
            
        if self.tabs.currentIndex() == 4 and self.processor_thread.latest_sniffed_data:
            self.bit_grid.update_data(self.processor_thread.latest_sniffed_data)

    def load_dbc_file(self):
        if not CANTOOLS_AVAILABLE:
            QMessageBox.warning(self, self.get_t("msg_err"), self.get_t("msg_no_cantools"))
            return
        filename, _ = QFileDialog.getOpenFileName(self, self.get_t("load_dbc"), "", "DBC Files (*.dbc);;All Files (*.*)")
        if filename:
            try:
                self.db = cantools.database.load_file(filename)
                self.dbc_filename = os.path.basename(filename)
                self.processor_thread.update_db(self.db)
                self.retranslate_ui()
                QMessageBox.information(self, self.get_t("msg_succ"), f"{self.get_t('msg_loaded')}{self.dbc_filename}")
            except Exception as e:
                QMessageBox.critical(self, self.get_t("msg_err"), f"{e}")

    def clear_table(self):
        self.table_model.clear_data()
        self.incoming_buffer.clear()
        self.processor_thread.clear_stats()
        self.chart_start_t = None
        if CHARTS_AVAILABLE: 
            for i in range(4):
                self.chart_points[i].clear()
                self.series_list[i].replace([])
        self.send_settings_to_thread()

    def toggle_pause(self):
        is_paused = self.pause_btn.isChecked()
        if is_paused:
            self.pause_btn.setText(self.get_t("resume"))
        else:
            self.pause_btn.setText(self.get_t("pause"))
        self.send_settings_to_thread()

    def change_language(self, index):
        self.current_lang = ["PL", "EN", "DE"][index]
        self.retranslate_ui()

    def retranslate_ui(self):
        t = TRANSLATIONS[self.current_lang]
        self.setWindowTitle(t["title"])
        self.load_dbc_btn.setText(t["load_dbc"])
        self.channel_label.setText(t["channel"])
        self.ip_label.setText(t["ip_label"])
        
        current_bus_idx = self.bus_filter_combo.currentIndex()
        self.bus_filter_combo.blockSignals(True) 
        self.bus_filter_combo.clear()
        self.bus_filter_combo.addItems([t["all_channels"], "CAN 1", "CAN 2"])
        if current_bus_idx != -1: self.bus_filter_combo.setCurrentIndex(current_bus_idx)
        self.bus_filter_combo.blockSignals(False)
        
        self.filter_label.setText(t["filter_id"])
        self.filter_input.setPlaceholderText(t["filter_ph"])
        self.delta_cb.setText(t["delta_time"])
        self.autoscroll_cb.setText(t["autoscroll"])
        self.clear_btn.setText(t["clear"])
        self.export_btn.setText(t["export_csv"])
        
        self.table_model.update_headers(t["headers"])
        self.stats_table.setHorizontalHeaderLabels(t["stats_headers"])
        self.tabs.setTabText(0, t["tab_monitor"])
        self.tabs.setTabText(1, t["tab_plots"])
        self.tabs.setTabText(2, t["tab_stats"])
        self.tabs.setTabText(3, t["tab_gen"])
        self.tabs.setTabText(4, t["tab_sniffer"])
        self.form_group.setTitle(t["tab_gen"])
        
        self.lbl_bus.setText(t["form_bus"])
        self.lbl_id.setText(t["form_id"])
        self.lbl_ext.setText(t["form_ext"])
        self.lbl_data.setText(t["form_data"])
        self.lbl_interval.setText(t["form_interval"])

        if CHARTS_AVAILABLE:
            for i in range(4):
                self.chart_labels_id[i].setText(f"{t['plot_id']} {i+1}:")
                self.chart_labels_b[i].setText(t["plot_byte"])
            
        self.sniffer_id_label.setText(t["sniffer_id"])
        self.send_once_btn.setText(t["btn_send_once"])
        self.start_cyclic_btn.setText(t["btn_start_cyclic"])
        self.stop_cyclic_btn.setText(t["btn_stop_cyclic"])
        
        with self.processor_thread.stats_lock:
            self.stats_label.setText(t["stats_summary"].format(self.processor_thread.total_frames, self.processor_thread.bus1_count, self.processor_thread.bus2_count, len(self.processor_thread.id_statistics), self.dbc_filename))

    def build_payload(self):
        target_bus = self.tx_bus_combo.currentIndex() + 1
        can_id = int(self.tx_id_input.text().strip(), 16)
        if self.tx_ext_id_cb.isChecked(): can_id |= 0x80000000 
        
        clean_hex = self.tx_data_input.text().replace(" ", "").replace("0x", "")
        data_bytes = bytes.fromhex(clean_hex)
        num_bytes = len(data_bytes)
        if num_bytes > 64: data_bytes = data_bytes[:64]; num_bytes = 64
        dlc_code, _ = len_to_dlc(num_bytes)
        return struct.pack(HEADER_FORMAT, 0, target_bus, can_id, dlc_code) + data_bytes.ljust(64, b'\x00')

    def send_can_frame_tcp(self, is_cyclic=False):
        try:
            payload = self.build_payload()
            success = self.tcp_thread.send_tcp_data(payload)
            if not success and not is_cyclic:
                QMessageBox.critical(self, self.get_t("msg_net_err"), "No TCP connection with ESP32!")
        except Exception as e:
            if not is_cyclic: QMessageBox.critical(self, self.get_t("msg_err"), f"{self.get_t('msg_build_err')}{e}")

    def start_cyclic_transmission(self):
        try:
            interval = max(5, int(self.tx_interval_input.text().strip()))
            payload = self.build_payload()
            self.cyclic_sender_thread = CyclicSenderThread(self.tcp_thread, payload, interval)
            self.cyclic_sender_thread.start()
            for widget in [self.tx_bus_combo, self.tx_id_input, self.tx_ext_id_cb, self.tx_data_input, self.tx_interval_input, self.start_cyclic_btn]: widget.setEnabled(False)
            self.stop_cyclic_btn.setEnabled(True)
        except ValueError: QMessageBox.warning(self, self.get_t("msg_err"), self.get_t("msg_bad_interval"))

    def stop_cyclic_transmission(self):
        if self.cyclic_sender_thread:
            self.cyclic_sender_thread.stop()
            self.cyclic_sender_thread = None
        for widget in [self.tx_bus_combo, self.tx_id_input, self.tx_ext_id_cb, self.tx_data_input, self.tx_interval_input, self.start_cyclic_btn]: widget.setEnabled(True)
        self.stop_cyclic_btn.setEnabled(False)

    def closeEvent(self, event):
        self.stop_cyclic_transmission()
        self.speed_timer.stop()
        self.scroll_timer.stop()
        self.fast_ui_timer.stop()
        self.tcp_thread.stop()
        self.processor_thread.stop()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyleSheet(INDUSTRIAL_STYLESHEET)
    viewer = CANViewerFullWindow()
    viewer.show()
    sys.exit(app.exec())