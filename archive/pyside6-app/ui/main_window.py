"""The viewer window: poll DeaDBeeF, resolve lyrics, drive the lyric column."""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QVBoxLayout,
    QWidget,
)

from core import artwork, deadbeef, lrc, resolver, seek
from core.tracker import Tracker
from ui import animations
from ui.backdrop import Backdrop
from ui.lyrics_view import LyricsView

# The UI ticks far more often than DeaDBeeF is polled; Tracker interpolates
# the position in between so highlighting still lands on the beat.
_TICK_MS = 60

_EMPTY = resolver.Resolved(lrc.Lyrics(), "")


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("LyricScope")
        self.resize(560, 780)

        self._tracker = Tracker(poll_interval=0.5)
        self._lyrics = _EMPTY
        self._art_cache: dict[str, QPixmap] = {}
        self._first_paint = True

        self._build()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(_TICK_MS)
        self._refresh_track()

    # -- construction ----------------------------------------------------

    def _build(self) -> None:
        self._backdrop = Backdrop()
        layout = QVBoxLayout(self._backdrop)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        layout.addWidget(self._build_header())
        self._view = LyricsView()
        self._view.lineClicked.connect(self._seek_to)
        layout.addWidget(self._view, 1)
        layout.addWidget(self._build_status())

        self.setCentralWidget(self._backdrop)

    def _build_header(self) -> QWidget:
        header = QWidget()
        header.setObjectName("header")
        box = QVBoxLayout(header)
        box.setContentsMargins(24, 16, 24, 16)
        box.setSpacing(3)

        self._title = QLabel("Nothing playing")
        self._title.setObjectName("trackTitle")
        self._title.setAlignment(Qt.AlignCenter)
        self._artist = QLabel("")
        self._artist.setObjectName("trackArtist")
        self._artist.setAlignment(Qt.AlignCenter)

        box.addWidget(self._title)
        box.addWidget(self._artist)
        return header

    def _build_status(self) -> QWidget:
        status = QWidget()
        status.setObjectName("status")
        box = QHBoxLayout(status)
        box.setContentsMargins(16, 8, 16, 8)

        self._source = QLabel("")
        self._source.setObjectName("sourceBadge")
        self._state = QLabel("Waiting for DeaDBeeF")
        self._state.setObjectName("statusLeft")
        self._clock = QLabel("")
        self._clock.setObjectName("statusRight")

        box.addWidget(self._source)
        box.addWidget(self._state)
        box.addStretch(1)
        box.addWidget(self._clock)
        return status

    # -- loop ------------------------------------------------------------

    def _tick(self) -> None:
        if self._tracker.due():
            if self._tracker.poll():
                self._refresh_track()
            self._refresh_status()

        if self._tracker.state is None or not self._lyrics.lyrics.synced:
            return

        self._view.set_current(self._lyrics.lyrics.index_at(self._tracker.position()))

    def _refresh_track(self) -> None:
        state = self._tracker.state
        if state is None:
            self._title.setText("Nothing playing")
            self._artist.setText("")
            self._source.setText("")
            self._lyrics = _EMPTY
            self._backdrop.show_pixmap(QPixmap(), animate=not self._first_paint)
            # now_playing() returns None both when the player is closed and
            # when it is open but stopped; only is_running() tells them apart.
            self._view.show_message(
                "Playback stopped."
                if deadbeef.is_running()
                else "Waiting for DeaDBeeF…"
            )
            self._first_paint = False
            return

        self._title.setText(state.title or "Unknown title")
        self._artist.setText(" — ".join(p for p in (state.artist, state.album) if p))

        self._lyrics = resolver.resolve(state.path, state.artist, state.title)
        # The art cross-fade and the lyric cascade are started together so
        # they overlap rather than following one another.
        self._backdrop.show_pixmap(
            self._artwork_for(state.path), animate=not self._first_paint
        )

        if self._lyrics:
            lines = self._lyrics.lyrics.lines
            synced = self._lyrics.lyrics.synced
            if self._first_paint:
                self._view.set_lines(lines, interactive=synced)
            else:
                self._view.transition_to(lines, interactive=synced)
            self._source.setText("synced" if synced else "unsynced")
        else:
            self._view.show_message("No lyrics found for this track.")
            self._source.setText("")

        self._first_paint = False

    def _artwork_for(self, path: str) -> QPixmap:
        """Blurred backdrop for a track, built once and cached."""
        if path in self._art_cache:
            return self._art_cache[path]

        pixmap = QPixmap()
        art = artwork.load(path)
        if art:
            blurred = artwork.blur(
                art.data,
                (self.width(), self.height()),
                animations.BLUR_DOWNSCALE,
                animations.BLUR_RADIUS,
            )
            if blurred:
                pixmap.loadFromData(blurred)

        # Cached even when empty, so a track without art isn't re-probed on
        # every track change back to it.
        self._art_cache[path] = pixmap
        return pixmap

    def _seek_to(self, seconds: float) -> None:
        if not seek.to_seconds(seconds):
            return
        # Re-poll straight away rather than waiting up to a poll interval,
        # so the highlight follows the click immediately.
        self._tracker.poll()
        if self._lyrics.lyrics.synced:
            self._view.set_current(self._lyrics.lyrics.index_at(self._tracker.position()))
        self._refresh_status()

    def _refresh_status(self) -> None:
        state = self._tracker.state
        if state is None:
            self._state.setText("Waiting for DeaDBeeF")
            self._clock.setText("")
            return

        detail = self._lyrics.source or "no lyrics"
        self._state.setText(f"{'Paused' if state.paused else 'Playing'} · {detail}")
        self._clock.setText(
            f"{_clock(self._tracker.position())} / {_clock(state.length)}"
        )


def _clock(seconds: float) -> str:
    seconds = max(0, int(seconds))
    return f"{seconds // 60}:{seconds % 60:02d}"
