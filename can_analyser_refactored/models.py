from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from PyQt6.QtGui import QColor, QFont, QPainter, QRegularExpressionValidator
from .constants import QAbstractTableModel, QColor, QModelIndex, Qt
class CANTableModel(QAbstractTableModel):
    def __init__(self, headers):
        super().__init__()
        self.headers = headers
        self.frames = []
        self.all_frames = []
        self.max_frames = 5000
        self.update_existing_ids = False
        self.enabled_ids = None

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
        if not new_frames:
            return

        self.all_frames.extend(new_frames)
        if self.max_frames is not None and len(self.all_frames) > self.max_frames:
            self.all_frames = self.all_frames[-self.max_frames:]
        self._rebuild_visible_frames()

    def _rebuild_visible_frames(self):
        source_frames = self.all_frames
        if self.enabled_ids is not None:
            source_frames = [
                frame for frame in source_frames
                if frame.get('clean_id') in self.enabled_ids
            ]

        if self.update_existing_ids:
            latest_by_key = {}
            order = []
            for frame in source_frames:
                key = (frame.get('node_id'), frame.get('clean_id'))
                if key not in latest_by_key:
                    order.append(key)
                latest_by_key[key] = frame
            visible_frames = [latest_by_key[key] for key in order]
        else:
            visible_frames = list(source_frames)

        self.beginResetModel()
        self.frames = visible_frames
        self.endResetModel()

    def set_enabled_ids(self, enabled_ids):
        self.enabled_ids = None if enabled_ids is None else set(enabled_ids)
        self._rebuild_visible_frames()

    def set_update_existing_ids(self, enabled):
        enabled = bool(enabled)
        if self.update_existing_ids == enabled:
            return

        self.update_existing_ids = enabled
        self._rebuild_visible_frames()

    def set_max_frames(self, max_frames):
        self.max_frames = max_frames
        if max_frames is not None and len(self.all_frames) > max_frames:
            self.all_frames = self.all_frames[-max_frames:]
        self._rebuild_visible_frames()

    def update_time_display(self, show_delta):
        previous_timestamps = {}
        for frame in self.frames:
            timestamp = frame.get('timestamp')
            if timestamp is None:
                continue

            display_key = (frame.get('node_id'), frame.get('clean_id'))
            if show_delta:
                previous_timestamp = previous_timestamps.get(display_key)
                if previous_timestamp is None:
                    frame['time'] = "0 ms"
                else:
                    delta_ms = timestamp - previous_timestamp
                    frame['time'] = f"+{delta_ms} ms" if delta_ms >= 0 else f"{delta_ms} ms"
            else:
                frame['time'] = f"{timestamp} ms"
            previous_timestamps[display_key] = timestamp

        if self.frames:
            first = self.index(0, 1)
            last = self.index(len(self.frames) - 1, 1)
            self.dataChanged.emit(first, last, [Qt.ItemDataRole.DisplayRole])

    def clear_data(self):
        self.beginResetModel()
        self.frames.clear()
        self.all_frames.clear()
        self.endResetModel()

    def update_headers(self, headers):
        self.headers = headers
        self.headerDataChanged.emit(Qt.Orientation.Horizontal, 0, len(self.headers) - 1)

