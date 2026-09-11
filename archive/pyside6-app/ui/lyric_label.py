"""A lyric line that can be scaled, faded and clicked.

Scaling is done at paint time, not by changing the font, because a font
cannot be resized smoothly. QFont.setPixelSize takes an int and
setPointSizeF is quantised back to whole pixels by the font engine, so
animating 0.80 -> 1.00 of a 26px line only ever produces six distinct
sizes: 21, 22, 23, 24, 25, 26. Over ~190ms that is a step every other
frame, which reads as a jump rather than a tween — and because each step
changes the label's height, the whole column below it reflows six times.

So the font is fixed at full size for every line and `scale` drives a
transform instead. Layout metrics never change, nothing below a growing
line moves, and the interpolation is continuous.

The text is drawn under that transform rather than resampled from a
bitmap, so it stays sharp at every scale.
"""

from __future__ import annotations

from PySide6.QtCore import Property, Qt, Signal
from PySide6.QtGui import QPainter
from PySide6.QtWidgets import QGraphicsOpacityEffect, QLabel, QWidget

from ui import animations

# Below this the transform is a no-op and native painting is used instead.
_NATIVE_EPSILON = 0.004


class LyricLabel(QLabel):
    clicked = Signal()

    def __init__(self, text: str, parent: QWidget | None = None) -> None:
        super().__init__(text, parent)
        self._scale = animations.NORMAL_SCALE
        self._opacity = 1.0
        self._effect: QGraphicsOpacityEffect | None = None
        self._seekable = False

        font = self.font()
        font.setPixelSize(animations.LINE_PX)
        self.setFont(font)

    # -- scale -----------------------------------------------------------

    def get_scale(self) -> float:
        return self._scale

    def set_scale(self, value: float) -> None:
        if abs(value - self._scale) < 0.0005:
            return
        self._scale = value
        self.update()

    scale = Property(float, get_scale, set_scale)

    # -- opacity ---------------------------------------------------------

    def get_opacity(self) -> float:
        return self._opacity

    def set_opacity(self, value: float) -> None:
        self._opacity = value
        if self._effect is None:
            # Fully opaque needs no effect at all; this is the common case.
            if value >= 0.999:
                return
            self._effect = QGraphicsOpacityEffect(self)
            self.setGraphicsEffect(self._effect)
        self._effect.setOpacity(value)

    opacity = Property(float, get_opacity, set_opacity)

    def clear_opacity_effect(self) -> None:
        """Drop the effect once a fade is done, restoring plain painting."""
        self._opacity = 1.0
        if self._effect is not None:
            self.setGraphicsEffect(None)
            self._effect = None

    # -- clicking --------------------------------------------------------

    def set_seekable(self, seekable: bool) -> None:
        """Only timestamped lines can be clicked to seek."""
        self._seekable = seekable
        self.setCursor(Qt.PointingHandCursor if seekable else Qt.ArrowCursor)

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        """Take the press so the matching release comes back here.

        QLabel ignores mouse events unless text interaction is enabled, and
        an ignored press propagates to the parent — taking the implicit
        mouse grab with it, so the release is delivered to the scroll area
        and the click is silently lost. Setting TextSelectableByMouse used
        to buy this acceptance as a side effect; now that the text is
        custom-painted and selection no longer draws, the acceptance is
        stated outright instead of riding on a feature that does nothing.
        """
        if self._seekable and event.button() == Qt.LeftButton:
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
        # Released inside the label, as a click. A drag that wanders off the
        # label is not one, so it is ignored.
        if (
            self._seekable
            and event.button() == Qt.LeftButton
            and self.rect().contains(event.position().toPoint())
        ):
            self.clicked.emit()
        super().mouseReleaseEvent(event)

    # -- painting --------------------------------------------------------

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        """Draw the text under a scale transform.

        The obvious implementation — snapshot the widget with render() and
        blit it scaled — is not allowed: render() inside a paint event is a
        recursive repaint, which Qt refuses ("Recursive repaint detected")
        and which leaves the painter dead. Drawing the text directly is both
        legal and better: the glyph outlines are transformed, so it stays
        vector-sharp at every scale instead of resampling a bitmap.
        """
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.setRenderHint(QPainter.TextAntialiasing, True)

        if self._scale < animations.CURRENT_SCALE - _NATIVE_EPSILON:
            # Scale about the centre so a line grows and shrinks in place
            # rather than drifting toward a corner.
            centre_x = self.width() / 2.0
            centre_y = self.height() / 2.0
            painter.translate(centre_x, centre_y)
            painter.scale(self._scale, self._scale)
            painter.translate(-centre_x, -centre_y)

        painter.setFont(self.font())
        # The stylesheet's colour arrives here through the palette, alpha
        # included, so the theme stays tokenized in styles.qss.
        painter.setPen(self.palette().color(self.foregroundRole()))
        painter.drawText(
            self.rect(),
            int(self.alignment() | Qt.TextWordWrap),
            self.text(),
        )
