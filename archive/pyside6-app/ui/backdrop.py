"""The blurred album-art background, and its cross-fade between tracks.

Both the outgoing and incoming pixmaps are painted every frame during a
change, the old one at 1-f and the new at f, so they overlap rather than
one following the other. A scrim is painted on top so lyrics stay legible
over bright covers.
"""

from __future__ import annotations

from PySide6.QtCore import Property, QPropertyAnimation, Qt
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import QWidget

from ui import animations

# Fallback when a track has no art at all.
_EMPTY_BG = QColor("#0f1115")


class Backdrop(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setAttribute(Qt.WA_StyledBackground, False)
        self._current = QPixmap()
        self._previous = QPixmap()
        self._fade = 1.0

        self._animation = QPropertyAnimation(self, b"fade", self)
        self._animation.setDuration(animations.BACKDROP_FADE_MS)
        self._animation.setEasingCurve(animations.BACKDROP_FADE_EASING)
        self._animation.finished.connect(self._drop_previous)

    # -- fade property ---------------------------------------------------

    def get_fade(self) -> float:
        return self._fade

    def set_fade(self, value: float) -> None:
        self._fade = value
        self.update()

    fade = Property(float, get_fade, set_fade)

    # -- api -------------------------------------------------------------

    def show_pixmap(self, pixmap: QPixmap, animate: bool = True) -> None:
        """Cross-fade to `pixmap` (which may be null for 'no art')."""
        self._animation.stop()
        self._previous = self._current
        self._current = pixmap

        if not animate or (self._previous.isNull() and self._current.isNull()):
            self._previous = QPixmap()
            self._fade = 1.0
            self.update()
            return

        self._fade = 0.0
        self._animation.setStartValue(0.0)
        self._animation.setEndValue(1.0)
        self._animation.start()

    def _drop_previous(self) -> None:
        self._previous = QPixmap()
        self.update()

    # -- painting --------------------------------------------------------

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        painter = QPainter(self)
        painter.fillRect(self.rect(), _EMPTY_BG)

        if not self._previous.isNull():
            painter.setOpacity(1.0 - self._fade)
            self._draw(painter, self._previous)

        if not self._current.isNull():
            painter.setOpacity(self._fade)
            self._draw(painter, self._current)

        painter.setOpacity(1.0)
        scrim = QColor(_EMPTY_BG)
        scrim.setAlphaF(animations.BACKDROP_SCRIM_ALPHA)
        painter.fillRect(self.rect(), scrim)

    def _draw(self, painter: QPainter, pixmap: QPixmap) -> None:
        # The pixmap is pre-cropped to the window's aspect, but the window
        # can be resized between track changes, so cover-scale here too.
        scaled = pixmap.scaled(
            self.size(), Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation
        )
        x = (scaled.width() - self.width()) // 2
        y = (scaled.height() - self.height()) // 2
        painter.drawPixmap(-x, -y, scaled)
