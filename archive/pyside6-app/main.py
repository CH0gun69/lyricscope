import sys

from PySide6.QtWidgets import QApplication

from ui import stylesheet
from ui.main_window import MainWindow

app = QApplication(sys.argv)

# Through the loader, not read straight off disk: styles.qss carries an
# @ACCENT@ token that has to be rendered before Qt sees it.
stylesheet.apply_to(app)

window = MainWindow()
window.show()

app.exec()
