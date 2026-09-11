"""The scrolling lyric column.

One label per line rather than a single rich-text block, because the
current line has to be measured (to centre it) and rescaled on its own
several times a second. Qt can do that to a widget cheaply; re-laying out
a whole document each tick is where this kind of view goes slow.

Track changes are deliberately blunt: the old labels are torn down
synchronously and the new ones fade in as a single block. An earlier
version staggered each line out and in, which needed per-line timers and
animation-finished callbacks to decide when widgets could be destroyed —
a lot of machinery whose failure mode is orphaned widgets. Tearing down
first means the container can only ever hold one track's lines, whatever
the user does to the skip button.
"""

from __future__ import annotations

from PySide6.QtCore import QPropertyAnimation, Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QFrame,
    QGraphicsOpacityEffect,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from core.lrc import Line
from ui import animations
from ui.lyric_label import LyricLabel

# How far down the viewport the active line sits, as a fraction of height.
# Slightly above centre reads better: there is more lyric ahead than behind.
_FOCUS = 0.42

# Distinct from -1, which is a real state: before the first timestamp there
# is genuinely no current line, and that is not the same as never having had
# one. Only the latter suppresses the expand animation.
_NO_CURRENT = -2


class LyricsView(QScrollArea):
    lineClicked = Signal(float)  # timestamp in seconds

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.setFrameShape(QFrame.NoFrame)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setObjectName("lyricsScroll")
        # Transparency here is done with stylesheet rules, not
        # WA_TranslucentBackground: on a child widget that attribute paints
        # transparent *black* over the backdrop on screen, while
        # QWidget.grab() still composites it correctly — so it looks right
        # in a grab and solid black in the actual window.
        self.viewport().setAutoFillBackground(False)

        self._body = QWidget()
        self._body.setObjectName("lyricsBody")
        self._layout = QVBoxLayout(self._body)
        self._layout.setContentsMargins(48, 0, 48, 0)
        self._layout.setSpacing(16)
        self._layout.setAlignment(Qt.AlignTop)
        self.setWidget(self._body)

        self._labels: list[LyricLabel] = []
        self._times: list[float | None] = []
        self._current = _NO_CURRENT

        self._scale_animations: list[QPropertyAnimation] = []

        self._fade_effect: QGraphicsOpacityEffect | None = None
        self._fade: QPropertyAnimation | None = None

        self._scroll = QPropertyAnimation(self.verticalScrollBar(), b"value", self)
        self._scroll.setDuration(animations.SCROLL_MS)
        self._scroll.setEasingCurve(animations.SCROLL_EASING)

        # Repaint driver. The column and the viewport are transparent so the
        # blurred backdrop shows through, which means nothing inside the
        # scroll area ever erases a background: when a label is rescaled or
        # destroyed, the pixels it leaves behind are not repainted, because
        # the widget that actually painted them is the Backdrop, outside this
        # scroll area. Marking the viewport dirty each frame makes Qt repaint
        # from that opaque ancestor down. Without it, a track change leaves a
        # smear of ghost text.
        self._repaint = QTimer(self)
        self._repaint.setInterval(16)
        self._repaint.timeout.connect(self._drive_repaint)

        # Padding so the first and last lines can still reach the focus
        # point instead of being pinned to the viewport edges.
        self._top_pad = QWidget()
        self._bottom_pad = QWidget()
        for pad in (self._top_pad, self._bottom_pad):
            pad.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Fixed)
            pad.setObjectName("lyricPad")

    # -- repainting ------------------------------------------------------

    def _start_repainting(self) -> None:
        if not self._repaint.isActive():
            self._repaint.start()

    def _drive_repaint(self) -> None:
        self.viewport().update()
        fading = self._fade is not None and self._fade.state() == QPropertyAnimation.Running
        scaling = any(
            a.state() == QPropertyAnimation.Running for a in self._scale_animations
        )
        if not (fading or scaling):
            # One last update has just been queued above, so the final frame
            # is clean before the driver goes quiet.
            self._repaint.stop()

    # -- population ------------------------------------------------------

    def clear(self) -> None:
        """Tear the column down immediately.

        setParent(None) before deleteLater() is the point of this method:
        deleteLater() alone defers destruction to the next event loop pass,
        so the old labels would still be children of the column while the
        new ones are added, and anything counting children right after a
        rebuild would see both tracks at once. Reparenting drops them from
        the container synchronously; deleteLater then frees them.
        """
        self._stop_fade()
        while self._layout.count():
            item = self._layout.takeAt(0)
            widget = item.widget()
            if widget is None:
                continue
            widget.setParent(None)
            if widget not in (self._top_pad, self._bottom_pad):
                widget.deleteLater()
        self._labels = []
        self._times = []
        self._current = _NO_CURRENT
        # The widgets are gone but their pixels are not; see _drive_repaint.
        self.viewport().update()

    def show_message(self, message: str) -> None:
        """Render a single centred line — 'no lyrics found', errors, etc."""
        self.clear()
        label = LyricLabel(message)
        label.setObjectName("placeholder")
        label.set_scale(animations.CURRENT_SCALE)
        label.setAlignment(Qt.AlignCenter)
        label.setWordWrap(True)
        self._layout.addWidget(self._top_pad)
        self._layout.addWidget(label)
        self._layout.addWidget(self._bottom_pad)
        self._resize_padding()

    def set_lines(self, lines: list[Line], interactive: bool) -> None:
        """Populate the column. `interactive` marks lyrics that will be synced."""
        self.clear()
        self._layout.addWidget(self._top_pad)
        for line in lines:
            label = LyricLabel(line.text or " ")
            label.setObjectName("lyricLine")
            label.setAlignment(Qt.AlignCenter)
            label.setWordWrap(True)
            # Unsynced lyrics have no current line, so render them all at
            # full strength rather than leaving the column dimmed — and at
            # full scale, so they paint natively instead of as a scaled
            # snapshot with nothing to gain from it.
            label.setProperty("state", "future" if interactive else "plain")
            if not interactive:
                label.set_scale(animations.CURRENT_SCALE)
            if interactive and line.time is not None:
                label.set_seekable(True)
                label.clicked.connect(
                    lambda t=line.time: self.lineClicked.emit(t)
                )
            self._labels.append(label)
            self._times.append(line.time)
            self._layout.addWidget(label)
        self._layout.addWidget(self._bottom_pad)
        self._resize_padding()
        # Lay out now rather than on the next event loop pass. Callers set
        # the current line immediately after this, and both the centring
        # maths and the first paint need real geometry — otherwise every
        # label still reports y() == 0.
        self._layout.activate()
        self.verticalScrollBar().setValue(0)

    # -- track change ----------------------------------------------------

    def transition_to(self, lines: list[Line], interactive: bool) -> None:
        """Swap in a new track's lyrics: hard teardown, then one quick fade.

        The teardown is synchronous and unconditional, so skipping tracks
        faster than the fade can finish is not a special case — the column
        holds the new lines before this method returns.
        """
        self.set_lines(lines, interactive)
        if lines:
            self._fade_in()

    def _fade_in(self) -> None:
        """Fade the whole column in once, rather than per line."""
        self._stop_fade()

        effect = QGraphicsOpacityEffect(self._body)
        self._body.setGraphicsEffect(effect)
        self._fade_effect = effect

        animation = QPropertyAnimation(effect, b"opacity", self)
        animation.setDuration(animations.LYRICS_FADE_MS)
        animation.setEasingCurve(animations.LYRICS_FADE_EASING)
        animation.setStartValue(0.0)
        animation.setEndValue(1.0)
        # Effects cost something to keep installed, and leaving one on the
        # column would render every later frame through it for nothing.
        animation.finished.connect(self._stop_fade)
        self._fade = animation
        animation.start()
        self._start_repainting()

    def _stop_fade(self) -> None:
        if self._fade is not None:
            self._fade.stop()
            self._fade = None
        if self._fade_effect is not None:
            self._body.setGraphicsEffect(None)
            self._fade_effect = None

    # -- current line ----------------------------------------------------

    def set_current(self, index: int, animate: bool = True) -> None:
        if index == self._current or not self._labels:
            return
        previous = self._current
        self._current = index

        # The first highlight after a rebuild has nothing to animate *from*:
        # every line is still at its resting scale, so tweening would show
        # the unfocused arrangement first and then grow into the right one —
        # a visible pop exactly when a track's lyrics appear. Snap instead,
        # so the first painted frame is already correct.
        if previous == _NO_CURRENT:
            animate = False

        for i, label in enumerate(self._labels):
            if label.property("state") == "plain":
                continue
            state = "current" if i == index else ("past" if i < index else "future")
            if label.property("state") != state:
                label.setProperty("state", state)
                # Qt only re-reads a stylesheet selector on a property
                # change if the style is explicitly repolished.
                label.style().unpolish(label)
                label.style().polish(label)

        if animate:
            self._animate_scale(previous, index)
        else:
            for i, label in enumerate(self._labels):
                label.set_scale(
                    animations.CURRENT_SCALE if i == index else animations.NORMAL_SCALE
                )

        if 0 <= index < len(self._labels):
            self._centre_on(self._labels[index], animate)

    def _animate_scale(self, outgoing: int, incoming: int) -> None:
        """Shrink the old current line while the new one grows, together."""
        for animation in self._scale_animations:
            animation.stop()
        self._scale_animations = []

        pairs = []
        if 0 <= outgoing < len(self._labels):
            pairs.append((self._labels[outgoing], animations.NORMAL_SCALE))
        if 0 <= incoming < len(self._labels):
            pairs.append((self._labels[incoming], animations.CURRENT_SCALE))

        for label, target in pairs:
            animation = QPropertyAnimation(label, b"scale", self)
            animation.setDuration(animations.LINE_TRANSITION_MS)
            animation.setEasingCurve(animations.LINE_TRANSITION_EASING)
            animation.setStartValue(label.get_scale())
            animation.setEndValue(target)
            animation.start()
            self._scale_animations.append(animation)
        if pairs:
            self._start_repainting()

        # Any line that isn't one of the two in play is snapped, in case a
        # previous animation was interrupted part-way.
        for i, label in enumerate(self._labels):
            if i not in (outgoing, incoming):
                label.set_scale(animations.NORMAL_SCALE)

    # -- scrolling -------------------------------------------------------

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._resize_padding()
        # Padding heights just changed, so every line moved. Re-centre
        # without animating; this fires during window drags.
        self.recentre(animate=False)

    def recentre(self, animate: bool = False) -> None:
        if 0 <= self._current < len(self._labels):
            self._centre_on(self._labels[self._current], animate)

    def _resize_padding(self) -> None:
        height = self.viewport().height()
        self._top_pad.setFixedHeight(int(height * _FOCUS))
        self._bottom_pad.setFixedHeight(int(height * (1.0 - _FOCUS)))

    def _centre_on(self, label: LyricLabel, animate: bool) -> None:
        # The first highlight usually lands before Qt has laid the column
        # out, when every label still reports y() == 0. Measuring then sends
        # the view to the top and, because the highlighted index doesn't
        # change again for a few seconds, it would stay there.
        self._layout.activate()

        target = label.y() - int(self.viewport().height() * _FOCUS) + label.height() // 2
        bar = self.verticalScrollBar()
        target = max(bar.minimum(), min(bar.maximum(), target))

        self._scroll.stop()
        if not animate:
            bar.setValue(target)
            return
        self._scroll.setStartValue(bar.value())
        self._scroll.setEndValue(target)
        self._scroll.start()
