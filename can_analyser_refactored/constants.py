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
    if os.path.exists(CONFIG_FILE):
        config.read(CONFIG_FILE)
    if not config.has_section("Network"):
        config.add_section("Network")
    config["Network"]["ip"] = ip
    config["Network"]["port"] = str(port)
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        config.write(f)

TCP_IP, TCP_PORT = load_config()

HEADER_FORMAT = "<IBIB"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
FRAME_SIZE = 74  # Strict size of log_frame_t from ESP32
CAN_CONFIG_MAGIC = 0x314E4143
CAN_CONTROL_FORMAT = "<IBIIB"
TCP_PACKET_HEADER_FORMAT = "<BH"
TCP_PACKET_TYPE_CAN_FRAME = 1
TCP_PACKET_TYPE_CAN_CONTROL = 2
TCP_PACKET_TYPE_MODE_CONTROL = 4
APP_MODE_QUERY_ONLY = 0xFF
APP_DISPLAY_MODE_BRIDGE = 0
APP_DISPLAY_MODE_SD_LOGGER = 1
APP_DISPLAY_MODE_TCP_SERVER = 2

# --- UI TRANSLATIONS ---
TRANSLATIONS = {
    "PL": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "load_log": "Otwórz Log", "channel": "Kanał:", "all_channels": "Wszystkie",
        "filter_id": "Filtr ID:", "filter_ph": "np. 123", "speed": "Prędkość:", "id_list": "Widoczne ID", "id_filter_enabled": "Filtruj listą ID", "select_all": "Wszystkie", "select_none": "Żadne",
        "pause": "Pauza", "resume": "Wznów", "autoscroll": "Auto-scroll", "delta_time": "Delta (Δt)", "update_existing_ids": "Aktualizuj ID",
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
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Wyeksportowano do: ",
        "tab_can_settings": "Ustawienia CAN",
        "mode_group": "Wybór trybu pracy (zdalny)", "mode_bridge": "Most CAN", "mode_sd": "Logger SD", "mode_tcp": "Serwer TCP",
        "btn_set_mode": "Ustaw tryb", "btn_query_mode": "Odpytaj tryb",
        "mode_remote_on": "Zdalna zmiana trybu: WŁĄCZONA", "mode_remote_off": "Zdalna zmiana trybu: WYŁĄCZONA (ustaw ostatni przełącznik DIP)",
        "mode_current": "Aktualny tryb: {}", "msg_mode_rejected": "ESP32 odrzucił zmianę trybu (zdalna kontrola wyłączona lub błąd)."
    },
    "EN": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "load_log": "Open Log", "channel": "Channel:", "all_channels": "All",
        "filter_id": "ID Filter:", "filter_ph": "e.g. 123", "speed": "Speed:", "id_list": "Visible IDs", "id_filter_enabled": "Filter by ID list", "select_all": "All", "select_none": "None",
        "pause": "Pause", "resume": "Resume", "autoscroll": "Auto-scroll", "delta_time": "Delta (Δt)", "update_existing_ids": "Update ID rows",
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
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Exported to: ",
        "tab_can_settings": "CAN Settings",
        "mode_group": "Operating Mode Selection (remote)", "mode_bridge": "CAN Bridge", "mode_sd": "SD Logger", "mode_tcp": "TCP Server",
        "btn_set_mode": "Set Mode", "btn_query_mode": "Query Mode",
        "mode_remote_on": "Remote mode control: ENABLED", "mode_remote_off": "Remote mode control: DISABLED (set last DIP switch)",
        "mode_current": "Current mode: {}", "msg_mode_rejected": "ESP32 rejected the mode change (remote control disabled or error)."
    },
    "DE": {
        "title": "ESP32 CAN-FD Analyzer", 
        "load_dbc": "📂 DBC", "load_log": "Log öffnen", "channel": "Kanal:", "all_channels": "Alle",
        "filter_id": "ID-Filter:", "filter_ph": "z.B. 123", "speed": "Geschwindigkeit:", "id_list": "Sichtbare IDs", "id_filter_enabled": "Nach ID-Liste filtern", "select_all": "Alle", "select_none": "Keine",
        "pause": "Pause", "resume": "Fortsetzen", "autoscroll": "Auto-Scroll", "delta_time": "Delta (Δt)", "update_existing_ids": "ID-Zeilen aktualisieren",
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
        "status_ok": "OK", "status_timeout": "TIMEOUT", "export_succ": "Exportiert nach: ",
        "tab_can_settings": "CAN Einstellungen",
        "mode_group": "Betriebsmodus-Auswahl (fernsteuerbar)", "mode_bridge": "CAN-Bridge", "mode_sd": "SD-Logger", "mode_tcp": "TCP-Server",
        "btn_set_mode": "Modus setzen", "btn_query_mode": "Modus abfragen",
        "mode_remote_on": "Fernsteuerung des Modus: AKTIV", "mode_remote_off": "Fernsteuerung des Modus: INAKTIV (letzten DIP-Schalter setzen)",
        "mode_current": "Aktueller Modus: {}", "msg_mode_rejected": "ESP32 hat die Moduswechsel-Anfrage abgelehnt (Fernsteuerung deaktiviert oder Fehler)."
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

