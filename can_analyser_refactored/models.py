from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from PyQt6.QtGui import QColor, QFont, QPainter, QRegularExpressionValidator
from .constants import QAbstractTableModel, QColor, QModelIndex, Qt
class CANTableModel(QAbstractTableModel):
    def __init__(self, headers):
        super().__init__()
        self.headers = headers
        self.frames = []
        self.max_frames = 5000

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

        new_frames = list(new_frames)
        if self.max_frames is not None and len(new_frames) > self.max_frames:
            new_frames = new_frames[-self.max_frames:]

        overflow = 0
        if self.max_frames is not None:
            overflow = max(0, len(self.frames) + len(new_frames) - self.max_frames)
        if overflow > 0:
            self.beginRemoveRows(QModelIndex(), 0, overflow - 1)
            del self.frames[:overflow]
            self.endRemoveRows()

        self.beginInsertRows(QModelIndex(), len(self.frames), len(self.frames) + len(new_frames) - 1)
        self.frames.extend(new_frames)
        self.endInsertRows()

    def set_max_frames(self, max_frames):
        self.max_frames = max_frames
        if max_frames is not None and len(self.frames) > max_frames:
            overflow = len(self.frames) - max_frames
            self.beginRemoveRows(QModelIndex(), 0, overflow - 1)
            del self.frames[:overflow]
            self.endRemoveRows()

    def update_time_display(self, show_delta):
        previous_timestamp = None
        for frame in self.frames:
            timestamp = frame.get('timestamp')
            if timestamp is None:
                continue

            if show_delta:
                if previous_timestamp is None:
                    frame['time'] = "0 ms"
                else:
                    delta_ms = timestamp - previous_timestamp
                    frame['time'] = f"+{delta_ms} ms" if delta_ms >= 0 else f"{delta_ms} ms"
            else:
                frame['time'] = f"{timestamp} ms"
            previous_timestamp = timestamp

        if self.frames:
            first = self.index(0, 1)
            last = self.index(len(self.frames) - 1, 1)
            self.dataChanged.emit(first, last, [Qt.ItemDataRole.DisplayRole])

    def clear_data(self):
        self.beginResetModel()
        self.frames.clear()
        self.endResetModel()

    def update_headers(self, headers):
        self.headers = headers
        self.headerDataChanged.emit(Qt.Orientation.Horizontal, 0, len(self.headers) - 1)

