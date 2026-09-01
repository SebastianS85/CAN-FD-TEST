import sys
from pathlib import Path

from PyQt6.QtWidgets import QApplication

if __package__:
    from .constants import INDUSTRIAL_STYLESHEET
    from .window import CANViewerFullWindow
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from can_analyser_refactored.constants import INDUSTRIAL_STYLESHEET
    from can_analyser_refactored.window import CANViewerFullWindow

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyleSheet(INDUSTRIAL_STYLESHEET)
    viewer = CANViewerFullWindow()
    viewer.show()
    sys.exit(app.exec())