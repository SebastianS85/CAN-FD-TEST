from PyQt6.QtCore import QThread, pyqtSignal, Qt, QTimer, QAbstractTableModel, QModelIndex, QPointF, QRegularExpression
from PyQt6.QtGui import QColor, QFont, QPainter, QRegularExpressionValidator
from PyQt6.QtWidgets import (
    QApplication, QHeaderView, QLabel, QTableView,
    QMainWindow, QPushButton, QTableWidget,
    QTableWidgetItem, QVBoxLayout, QWidget, QHBoxLayout, 
    QCheckBox, QLineEdit, QComboBox, QMessageBox, QTabWidget,
    QFileDialog, QGroupBox, QFormLayout, QScrollArea, QFrame
)
from .constants import QColor, QFont, QPainter, QTimer, QWidget
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

