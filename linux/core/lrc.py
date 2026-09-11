"""Parse LRC lyrics and match them to a playback position.

The approach follows deadbeef-lyricbar's plugin (src/utils.cpp), which is
what DeaDBeeF users' .lrc files are written against:

  * a timestamp is [mm:ss.xx] and means "this line starts here"
  * lines are sorted by time, then the line to highlight is the last one
    whose time has already passed
  * a file counts as *synced* only if most of its lines carry timestamps

The parsing itself is done with a regex rather than lyricbar's fixed
character offsets (it tests for '[' at i, ':' at i+3, '.' at i+6), because
that shape rejects perfectly ordinary variants: [m:ss.xx], [mm:ss] with no
fraction, and the millisecond form [mm:ss.xxx] that LRCLIB serves.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

# [mm:ss.xx] / [mm:ss.xxx] / [mm:ss] — minutes may be 1-3 digits so that
# tracks past the hour mark still parse.
_TIMESTAMP = re.compile(r"\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\]")

# [ar:...], [ti:...], [offset:-500] and friends. Distinguished from a real
# timestamp by having a non-digit key.
_ID_TAG = re.compile(r"^\s*\[([a-zA-Z#]+):(.*)\]\s*$")

# Share of lines that must be timestamped before we treat a file as synced.
# Lifted from lyricbar, which uses >75% so that a stray header or a blank
# line doesn't demote an otherwise-synced file to plain text.
_SYNCED_THRESHOLD = 0.75


@dataclass(frozen=True)
class Line:
    """One lyric line. `time` is None for unsynced (plain text) lyrics."""

    time: float | None
    text: str


@dataclass
class Lyrics:
    lines: list[Line] = field(default_factory=list)
    synced: bool = False
    tags: dict[str, str] = field(default_factory=dict)

    def __bool__(self) -> bool:
        return bool(self.lines)

    @property
    def text(self) -> str:
        return "\n".join(line.text for line in self.lines)

    def index_at(self, position: float) -> int:
        """Index of the line that should be highlighted at `position`.

        Returns -1 before the first timestamp, so callers can render a
        lead-in state rather than falsely highlighting line 0 during an
        intro. Unsynced lyrics have no current line, so they also give -1.
        """
        if not self.synced:
            return -1

        found = -1
        for i, line in enumerate(self.lines):
            if line.time is None:
                continue
            if line.time <= position:
                found = i
            else:
                # Lines are sorted, so nothing further can match.
                break
        return found


def parse(raw: str) -> Lyrics:
    """Parse LRC (or plain text) into `Lyrics`."""
    if not raw or not raw.strip():
        return Lyrics()

    tags: dict[str, str] = {}
    entries: list[Line] = []
    plain: list[str] = []
    total = 0

    for raw_line in raw.splitlines():
        line = raw_line.rstrip()
        if not line.strip():
            continue

        id_tag = _ID_TAG.match(line)
        if id_tag and not id_tag.group(1).isdigit():
            tags[id_tag.group(1).lower()] = id_tag.group(2).strip()
            continue

        total += 1
        stamps = list(_TIMESTAMP.finditer(line))
        if not stamps:
            plain.append(line.strip())
            continue

        # Text is whatever follows the final timestamp. A line may carry
        # several ([00:01.00][01:30.00] Chorus) — LRC's way of saying the
        # same words repeat, so it becomes one entry per timestamp.
        text = line[stamps[-1].end():].strip()
        for stamp in stamps:
            entries.append(Line(time=_stamp_seconds(stamp), text=text))
            plain.append(text)

    if not entries:
        return Lyrics(
            lines=[Line(time=None, text=t) for t in plain],
            synced=False,
            tags=tags,
        )

    synced = total > 0 and (len(entries) / total) >= _SYNCED_THRESHOLD
    if not synced:
        return Lyrics(
            lines=[Line(time=None, text=t) for t in plain],
            synced=False,
            tags=tags,
        )

    offset = _offset_seconds(tags)
    if offset:
        entries = [Line(time=max(0.0, ln.time + offset), text=ln.text) for ln in entries]

    entries.sort(key=lambda ln: ln.time)
    return Lyrics(lines=entries, synced=True, tags=tags)


def _stamp_seconds(match: re.Match) -> float:
    minutes = int(match.group(1))
    seconds = int(match.group(2))
    fraction = match.group(3)

    total = minutes * 60 + seconds
    if fraction:
        # "5" means .5s, "50" means .50s, "500" means .500s — scale by width
        # instead of assuming centiseconds, so LRCLIB's millisecond stamps
        # don't come out 10x too large.
        total += int(fraction) / (10 ** len(fraction))
    return float(total)


def _offset_seconds(tags: dict[str, str]) -> float:
    """[offset:] is in milliseconds and is positive when lyrics are early."""
    raw = tags.get("offset")
    if not raw:
        return 0.0
    try:
        # The tag shifts lyrics *earlier* for positive values, so the sign
        # is inverted when applied to line times.
        return -int(raw.strip()) / 1000.0
    except ValueError:
        return 0.0
