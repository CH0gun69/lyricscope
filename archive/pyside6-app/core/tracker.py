"""Keep a current playback position between polls.

Polling DeaDBeeF spawns a process (~1-5ms), so doing it at the rate the UI
redraws would be wasteful. Instead the CLI is polled a couple of times a
second for truth, and the position is carried forward with a monotonic
clock in between. Every poll resnaps to the real value, so drift can never
accumulate beyond one poll interval.

This also papers over the resolution of the CLI itself: %playback_time_seconds%
is reported to 1/100s, which is exactly LRC's precision but still a visible
step if you highlight straight from it.
"""

from __future__ import annotations

import time

from core.deadbeef import NowPlaying, now_playing


class Tracker:
    """Tracks the playing track and an interpolated playback position."""

    def __init__(self, poll_interval: float = 0.5) -> None:
        self.poll_interval = poll_interval
        self.state: NowPlaying | None = None
        self._polled_at = 0.0
        self._last_poll = 0.0

    @property
    def track_id(self) -> str:
        """Identity of the current track, for spotting a track change."""
        return self.state.path if self.state else ""

    def due(self, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        return (now - self._last_poll) >= self.poll_interval

    def poll(self) -> bool:
        """Refresh from DeaDBeeF. True if the track changed."""
        previous = self.track_id
        self.state = now_playing()
        self._polled_at = time.monotonic()
        self._last_poll = self._polled_at
        return self.track_id != previous

    def position(self, now: float | None = None) -> float:
        """Best estimate of the current position, in seconds."""
        if self.state is None:
            return 0.0
        if self.state.paused:
            # A paused player's position is already exact and isn't moving.
            return self.state.position

        now = time.monotonic() if now is None else now
        elapsed = max(0.0, now - self._polled_at)
        estimate = self.state.position + elapsed

        # Don't run off the end of a track while waiting for the poll that
        # will tell us the next one started.
        if self.state.length > 0:
            estimate = min(estimate, self.state.length)
        return estimate
