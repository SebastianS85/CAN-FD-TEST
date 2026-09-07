import struct
import os
import logging
import csv
from collections import deque
from PyQt6.QtCore import QByteArray, QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from PyQt6.QtGui import QColor, QFont, QPainter, QRegularExpressionValidator
from PyQt6.QtWidgets import (
    QApplication, QHeaderView, QLabel, QTableView,
    QMainWindow, QPushButton, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget, QHBoxLayout, 
    QCheckBox, QLineEdit, QComboBox, QMessageBox, QTabWidget,
    QFileDialog, QGroupBox, QFormLayout, QScrollArea, QFrame
)
from .constants import APP_DISPLAY_MODE_BRIDGE, APP_DISPLAY_MODE_SD_LOGGER, APP_DISPLAY_MODE_TCP_SERVER, CAN_CONFIG_MAGIC, CAN_CONTROL_FORMAT, CANTOOLS_AVAILABLE, CHARTS_AVAILABLE, FRAME_SIZE, HEADER_FORMAT, TCP_PACKET_HEADER_FORMAT, TCP_PACKET_TYPE_CAN_CONTROL, TCP_PACKET_TYPE_CAN_FRAME, TCP_PACKET_TYPE_MODE_CONTROL, QCheckBox, QColor, QComboBox, QFileDialog, QFont, QFormLayout, QFrame, QGroupBox, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow, QMessageBox, QPainter, QPointF, QPushButton, QRegularExpression, QRegularExpressionValidator, QScrollArea, QTabWidget, QTableView, QTableWidget, QTableWidgetItem, QTimer, QVBoxLayout, QWidget, Qt, TCP_IP, TCP_PORT, TRANSLATIONS, csv, deque, len_to_dlc, logging, os, save_config, struct

if CANTOOLS_AVAILABLE:
    from .constants import cantools

if CHARTS_AVAILABLE:
    from .constants import QChart, QChartView, QLineSeries, QValueAxis
from .log_io import LogFileLoaderThread
from .models import CANTableModel
from .processing import DataProcessorThread
from .settings import load_settings, save_settings
from .transport import CyclicSenderThread, TCPReceiverThread
from .widgets import BitGridWidget
class CANViewerFullWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.settings = load_settings()
        self.current_lang = self.settings["language"] if self.settings["language"] in TRANSLATIONS else "PL"
        self.incoming_buffer = deque()
        self.cyclic_sender_thread = None
        self.log_loader_thread = None
        self.db = None
        self.dbc_filename = "-"
        self.id_filter_checkboxes = {}

        self.chart_points = [deque() for _ in range(4)]
        self.chart_start_t = None
        self._mode_remote_enabled = False
        self._current_mode = None

        self.setup_ui()

        self.tcp_thread = TCPReceiverThread(TCP_IP, TCP_PORT, self.incoming_buffer)
        self.tcp_thread.connection_status.connect(self.update_connection_status)
        self.tcp_thread.start()

        self.processor_thread = DataProcessorThread(self.incoming_buffer)
        self.processor_thread.frames_ready.connect(self.on_frames_ready)
        self.processor_thread.start()

        self.retranslate_ui()
        self.restore_ui_settings()

        self.filter_input.textChanged.connect(self.send_settings_to_thread)
        self.bus_filter_combo.currentIndexChanged.connect(self.send_settings_to_thread)
        self.delta_cb.stateChanged.connect(self.update_delta_display)
        self.update_existing_ids_cb.stateChanged.connect(self.update_existing_ids)
        self.id_filter_enabled_cb.stateChanged.connect(self.send_settings_to_thread)
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

    def restore_ui_settings(self):
        language_index = {"PL": 0, "EN": 1, "DE": 2}[self.current_lang]
        self.lang_combo.blockSignals(True)
        self.lang_combo.setCurrentIndex(language_index)
        self.lang_combo.blockSignals(False)
        self.filter_input.setText(str(self.settings["filter_text"]))
        self.bus_filter_combo.setCurrentIndex(int(self.settings["bus_filter_index"]))
        self.delta_cb.setChecked(bool(self.settings["delta_enabled"]))
        self.id_filter_enabled_cb.setChecked(bool(self.settings.get("id_filter_enabled", False)))
        self.update_existing_ids_cb.setChecked(bool(self.settings.get("update_existing_ids", False)))
        self.autoscroll_cb.setChecked(bool(self.settings["autoscroll_enabled"]))
        self.tabs.setCurrentIndex(int(self.settings["active_tab_index"]))

        geometry = self.settings["window_geometry"]
        if isinstance(geometry, str):
            self.restoreGeometry(QByteArray.fromBase64(geometry.encode("ascii")))

    def save_ui_settings(self):
        save_settings({
            "language": self.current_lang,
            "filter_text": self.filter_input.text(),
            "bus_filter_index": self.bus_filter_combo.currentIndex(),
            "delta_enabled": self.delta_cb.isChecked(),
            "id_filter_enabled": self.id_filter_enabled_cb.isChecked(),
            "update_existing_ids": self.update_existing_ids_cb.isChecked(),
            "autoscroll_enabled": self.autoscroll_cb.isChecked(),
            "active_tab_index": self.tabs.currentIndex(),
            "window_geometry": bytes(self.saveGeometry().toBase64()).decode("ascii"),
        })

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

        self.load_log_btn = QPushButton()
        self.load_log_btn.clicked.connect(self.load_log_file)
        
        self.channel_label = QLabel()
        self.bus_filter_combo = QComboBox()
        self.filter_label = QLabel()
        self.filter_input = QLineEdit()
        self.filter_input.setMaximumWidth(80)
        self.id_filter_enabled_cb = QCheckBox()
        
        self.speed_label = QLabel()
        self.speed_label.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
        self.speed_label.setStyleSheet("color: #ffb74d;")
        
        self.pause_btn = QPushButton()
        self.pause_btn.setCheckable(True)
        self.pause_btn.setMinimumWidth(80)
        self.pause_btn.clicked.connect(self.toggle_pause)
        
        self.delta_cb = QCheckBox()
        self.update_existing_ids_cb = QCheckBox()
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
        top_layout.addWidget(self.load_log_btn)
        top_layout.addWidget(self.channel_label)
        top_layout.addWidget(self.bus_filter_combo)
        top_layout.addWidget(self.filter_label)
        top_layout.addWidget(self.filter_input)
        top_layout.addStretch()
        top_layout.addWidget(self.speed_label)
        top_layout.addWidget(self.pause_btn)
        top_layout.addWidget(self.delta_cb)
        top_layout.addWidget(self.update_existing_ids_cb)
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
        monitor_content = QHBoxLayout()
        monitor_content.addWidget(self.table, 1)

        self.id_filter_group = QGroupBox()
        self.id_filter_group.setFixedWidth(190)
        id_filter_layout = QVBoxLayout(self.id_filter_group)
        id_filter_buttons = QHBoxLayout()
        self.select_all_ids_btn = QPushButton()
        self.select_none_ids_btn = QPushButton()
        self.select_all_ids_btn.clicked.connect(lambda: self.set_all_id_filters(True))
        self.select_none_ids_btn.clicked.connect(lambda: self.set_all_id_filters(False))
        id_filter_buttons.addWidget(self.select_all_ids_btn)
        id_filter_buttons.addWidget(self.select_none_ids_btn)
        id_filter_layout.addLayout(id_filter_buttons)
        id_filter_layout.addWidget(self.id_filter_enabled_cb)

        self.id_filter_scroll = QScrollArea()
        self.id_filter_scroll.setWidgetResizable(True)
        self.id_filter_widget = QWidget()
        self.id_filter_list_layout = QVBoxLayout(self.id_filter_widget)
        self.id_filter_list_layout.setAlignment(Qt.AlignmentFlag.AlignTop)
        self.id_filter_scroll.setWidget(self.id_filter_widget)
        id_filter_layout.addWidget(self.id_filter_scroll)
        monitor_content.addWidget(self.id_filter_group)
        monitor_layout.addLayout(monitor_content)
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

        can_settings_tab = QWidget()
        can_settings_layout = QVBoxLayout(can_settings_tab)
        self.can1_settings_group = QGroupBox()
        self.can2_settings_group = QGroupBox()
        self.can1_arb_bitrate_combo, self.can1_data_bitrate_combo = self.add_can_settings_controls(
            can_settings_layout, self.can1_settings_group, 500000, 0, 1)
        self.can2_arb_bitrate_combo, self.can2_data_bitrate_combo = self.add_can_settings_controls(
            can_settings_layout, self.can2_settings_group, 1000000, 2000000, 2)

        self.mode_group = QGroupBox()
        mode_layout = QVBoxLayout(self.mode_group)
        self.mode_remote_status_label = QLabel()
        self.mode_current_label = QLabel()
        mode_layout.addWidget(self.mode_remote_status_label)
        mode_layout.addWidget(self.mode_current_label)
        mode_ctrl_layout = QHBoxLayout()
        self.mode_combo = QComboBox()
        mode_ctrl_layout.addWidget(self.mode_combo)
        self.set_mode_btn = QPushButton()
        self.set_mode_btn.clicked.connect(self.set_remote_mode)
        mode_ctrl_layout.addWidget(self.set_mode_btn)
        mode_layout.addLayout(mode_ctrl_layout)
        can_settings_layout.addWidget(self.mode_group)
        for mode_value in (APP_DISPLAY_MODE_BRIDGE, APP_DISPLAY_MODE_SD_LOGGER, APP_DISPLAY_MODE_TCP_SERVER):
            self.mode_combo.addItem("", mode_value)

        can_settings_layout.addStretch()
        self.tabs.addTab(can_settings_tab, "")

        sniffer_tab = QWidget()
        self.sniffer_tab = sniffer_tab
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

    def add_can_settings_controls(self, parent_layout, group, arbitration_bitrate, data_bitrate, node_id):
        form_layout = QFormLayout(group)
        arbitration_combo = QComboBox()
        data_combo = QComboBox()
        for label, value in [("125 kbit/s", 125000), ("250 kbit/s", 250000),
                             ("500 kbit/s", 500000), ("800 kbit/s", 800000),
                             ("1 Mbit/s", 1000000)]:
            arbitration_combo.addItem(label, value)
        for label, value in [("Classic CAN", 0), ("1 Mbit/s", 1000000),
                             ("2 Mbit/s", 2000000), ("4 Mbit/s", 4000000),
                             ("5 Mbit/s", 5000000)]:
            data_combo.addItem(label, value)
        arbitration_combo.setCurrentIndex(arbitration_combo.findData(arbitration_bitrate))
        data_combo.setCurrentIndex(data_combo.findData(data_bitrate))
        listen_only_cb = QCheckBox()
        apply_button = QPushButton()
        apply_button.clicked.connect(lambda: self.send_can_settings(node_id, arbitration_combo, data_combo, listen_only_cb))
        form_layout.addRow("Arbitration bitrate:", arbitration_combo)
        form_layout.addRow("Data bitrate:", data_combo)
        form_layout.addRow("Listen only:", listen_only_cb)
        form_layout.addRow("", apply_button)
        if node_id == 1:
            self.can1_apply_settings_btn = apply_button
            self.can1_listen_only_cb = listen_only_cb
        else:
            self.can2_apply_settings_btn = apply_button
            self.can2_listen_only_cb = listen_only_cb
        parent_layout.addWidget(group)
        return arbitration_combo, data_combo


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
        is_paused = False
        
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

        enabled_ids = self.get_enabled_ids() if self.id_filter_enabled_cb.isChecked() else None
        self.table_model.set_enabled_ids(enabled_ids)
        self.processor_thread.update_settings(is_paused, filter_text, bus_idx, is_delta, chart_targets, sniffer_id, enabled_ids)

    def get_enabled_ids(self):
        if not self.id_filter_checkboxes:
            return None
        return {
            can_id for can_id, checkbox in self.id_filter_checkboxes.items()
            if checkbox.isChecked()
        }

    def update_id_filter_panel(self):
        with self.processor_thread.stats_lock:
            discovered_ids = sorted(self.processor_thread.id_statistics)

        for can_id in discovered_ids:
            if can_id in self.id_filter_checkboxes:
                continue
            checkbox = QCheckBox(f"0x{can_id:03X}")
            checkbox.setChecked(True)
            checkbox.stateChanged.connect(self.send_settings_to_thread)
            self.id_filter_checkboxes[can_id] = checkbox
            self.id_filter_list_layout.addWidget(checkbox)
        if discovered_ids:
            self.send_settings_to_thread()

    def set_all_id_filters(self, enabled):
        for checkbox in self.id_filter_checkboxes.values():
            checkbox.setChecked(enabled)
        self.send_settings_to_thread()

    def on_frames_ready(self, processed_frames):
        self.table_model.add_frames(processed_frames)

    def check_autoscroll(self):
        if (not self.pause_btn.isChecked() and self.autoscroll_cb.isChecked() and
                self.tabs.currentIndex() == 0):
            self.table.scrollToBottom()

    def update_delta_display(self, _state=None):
        self.send_settings_to_thread()
        self.table_model.update_time_display(self.delta_cb.isChecked())

    def update_existing_ids(self, _state=None):
        self.table_model.set_update_existing_ids(self.update_existing_ids_cb.isChecked())

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
        self.update_id_filter_panel()
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
            
        if self.tabs.currentWidget() is self.sniffer_tab and self.processor_thread.latest_sniffed_data:
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

    def load_log_file(self):
        if self.log_loader_thread and self.log_loader_thread.isRunning():
            return

        filename, _ = QFileDialog.getOpenFileName(
            self,
            self.get_t("load_log"),
            "",
            "CAN Logs (*.bin *.asc);;ESP32 Binary Logs (*.bin);;SavvyCAN ASCII Logs (*.asc);;All Files (*.*)",
        )
        if not filename:
            return

        if not self.pause_btn.isChecked():
            self.pause_btn.setChecked(True)
            self.toggle_pause()
        self.table_model.set_max_frames(None)
        self.clear_table()
        self.load_log_btn.setEnabled(False)
        self.log_loader_thread = LogFileLoaderThread(filename)
        self.log_loader_thread.frames_loaded.connect(self.incoming_buffer.extend)
        self.log_loader_thread.load_finished.connect(self.on_log_load_finished)
        self.log_loader_thread.load_failed.connect(self.on_log_load_failed)
        self.log_loader_thread.start()

    def on_log_load_finished(self, frame_count):
        self.load_log_btn.setEnabled(True)
        QMessageBox.information(self, self.get_t("msg_succ"), f"Loaded {frame_count:,} CAN frames.")

    def on_log_load_failed(self, message):
        self.load_log_btn.setEnabled(True)
        QMessageBox.critical(self, self.get_t("msg_err"), message)

    def clear_table(self):
        self.table_model.clear_data()
        self.incoming_buffer.clear()
        self.processor_thread.clear_stats()
        for checkbox in self.id_filter_checkboxes.values():
            checkbox.deleteLater()
        self.id_filter_checkboxes.clear()
        self.chart_start_t = None
        if CHARTS_AVAILABLE: 
            for i in range(4):
                self.chart_points[i].clear()
                self.series_list[i].replace([])
        self.send_settings_to_thread()

    def toggle_pause(self):
        is_paused = self.pause_btn.isChecked()
        self.tcp_thread.set_connection_enabled(not is_paused)
        if is_paused:
            self.autoscroll_cb.setChecked(False)
            self.pause_btn.setText(self.get_t("resume"))
        else:
            self.table_model.set_max_frames(5000)
            self.pause_btn.setText(self.get_t("pause"))
        self.send_settings_to_thread()

    def change_language(self, index):
        self.current_lang = ["PL", "EN", "DE"][index]
        self.retranslate_ui()

    def retranslate_ui(self):
        t = TRANSLATIONS[self.current_lang]
        self.setWindowTitle(t["title"])
        self.load_dbc_btn.setText(t["load_dbc"])
        self.load_log_btn.setText(t["load_log"])
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
        self.id_filter_group.setTitle(t["id_list"])
        self.select_all_ids_btn.setText(t["select_all"])
        self.select_none_ids_btn.setText(t["select_none"])
        self.id_filter_enabled_cb.setText(t["id_filter_enabled"])
        self.delta_cb.setText(t["delta_time"])
        self.update_existing_ids_cb.setText(t["update_existing_ids"])
        self.autoscroll_cb.setText(t["autoscroll"])
        self.pause_btn.setText(t["resume"] if self.pause_btn.isChecked() else t["pause"])
        self.clear_btn.setText(t["clear"])
        self.export_btn.setText(t["export_csv"])
        
        self.table_model.update_headers(t["headers"])
        self.stats_table.setHorizontalHeaderLabels(t["stats_headers"])
        self.tabs.setTabText(0, t["tab_monitor"])
        self.tabs.setTabText(1, t["tab_plots"])
        self.tabs.setTabText(2, t["tab_stats"])
        self.tabs.setTabText(3, t["tab_gen"])
        self.tabs.setTabText(4, t["tab_can_settings"])
        self.tabs.setTabText(5, t["tab_sniffer"])
        self.mode_group.setTitle(t["mode_group"])
        self.set_mode_btn.setText(t["btn_set_mode"])
        mode_labels = {APP_DISPLAY_MODE_BRIDGE: t["mode_bridge"], APP_DISPLAY_MODE_SD_LOGGER: t["mode_sd"],
                       APP_DISPLAY_MODE_TCP_SERVER: t["mode_tcp"]}
        for i in range(self.mode_combo.count()):
            self.mode_combo.setItemText(i, mode_labels[self.mode_combo.itemData(i)])
        self.mode_remote_status_label.setText(
            t["mode_remote_on"] if getattr(self, "_mode_remote_enabled", False) else t["mode_remote_off"]
        )
        current_mode = getattr(self, "_current_mode", None)
        self.mode_current_label.setText(
            t["mode_current"].format(mode_labels.get(current_mode, "-"))
        )
        self.form_group.setTitle(t["tab_gen"])
        self.can1_settings_group.setTitle("CAN 1")
        self.can2_settings_group.setTitle("CAN 2")
        self.can1_apply_settings_btn.setText("Apply CAN 1")
        self.can2_apply_settings_btn.setText("Apply CAN 2")
        
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
        frame = struct.pack(HEADER_FORMAT, 0, target_bus, can_id, dlc_code) + data_bytes.ljust(64, b'\x00')
        return struct.pack(TCP_PACKET_HEADER_FORMAT, TCP_PACKET_TYPE_CAN_FRAME, FRAME_SIZE) + frame

    def send_can_frame_tcp(self, is_cyclic=False):
        try:
            payload = self.build_payload()
            success = self.tcp_thread.send_tcp_data(payload)
            if not success and not is_cyclic:
                QMessageBox.critical(self, self.get_t("msg_net_err"), "No TCP connection with ESP32!")
        except Exception as e:
            if not is_cyclic: QMessageBox.critical(self, self.get_t("msg_err"), f"{self.get_t('msg_build_err')}{e}")

    def send_can_settings(self, node_id, arbitration_combo, data_combo, listen_only_cb):
        if self.cyclic_sender_thread:
            self.stop_cyclic_transmission()
        arbitration_bitrate = arbitration_combo.currentData()
        data_bitrate = data_combo.currentData()
        command = struct.pack(CAN_CONTROL_FORMAT, CAN_CONFIG_MAGIC, node_id,
                      arbitration_bitrate, data_bitrate, 1 if listen_only_cb.isChecked() else 0)
        payload = struct.pack(TCP_PACKET_HEADER_FORMAT, TCP_PACKET_TYPE_CAN_CONTROL, len(command)) + command
        if not self.tcp_thread.send_tcp_control_command(payload):
            QMessageBox.critical(
                self,
                self.get_t("msg_net_err"),
                "ESP32 did not confirm the CAN configuration. Rebuild and flash the latest firmware."
            )

    def apply_mode_response(self, result):
        if result is None:
            QMessageBox.critical(self, self.get_t("msg_net_err"), "No TCP connection with ESP32!")
            return None
        ack_ok, mode, remote_enabled = result
        self._mode_remote_enabled = remote_enabled
        self._current_mode = mode
        self.retranslate_ui()
        if not ack_ok:
            QMessageBox.critical(self, self.get_t("msg_net_err"), self.get_t("msg_mode_rejected"))
        return result

    def set_remote_mode(self):
        requested_mode = self.mode_combo.currentData()
        command = struct.pack("<B", requested_mode)
        payload = struct.pack(TCP_PACKET_HEADER_FORMAT, TCP_PACKET_TYPE_MODE_CONTROL, len(command)) + command
        self.apply_mode_response(self.tcp_thread.send_tcp_mode_command(payload))

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
        self.save_ui_settings()
        self.stop_cyclic_transmission()
        self.speed_timer.stop()
        self.scroll_timer.stop()
        self.fast_ui_timer.stop()
        self.tcp_thread.stop()
        self.processor_thread.stop()
        if self.log_loader_thread:
            self.log_loader_thread.stop()
