"""Read DeaDBeeF's playback state from the outside.

Everything here goes through `deadbeef --nowplaying-tf`, the player's own
CLI. That forwards the request over DeaDBeeF's IPC socket
($XDG_RUNTIME_DIR/deadbeef/socket) to the already-running instance and
prints the answer. Nothing is installed into DeaDBeeF, no plugin is
required, and its config is never written.

Two things about that CLI decided the shape of this module:

1. Calling it when no instance is reachable does not fail — the binary
   goes on to `server_start` and binds the socket itself, i.e. it LAUNCHES
   the player. A viewer polling twice a second must therefore never call
   it blind; `is_running()` gates every poll.

2. Title formatting is built for one-line track titles, so a multi-line
   tag value comes back flattened with the newlines replaced. That makes
   %lyrics% useless for real lyrics (a 38-line tag came back as one line),
   which is why embedded lyrics are read from the file instead — see
   core.resolver.
"""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass

# DeaDBeeF's main thread sets its name to "deadbeef-main", so that — not
# "deadbeef" — is what shows up in /proc/<pid>/comm. `pgrep -x deadbeef`
# silently matches nothing even while the player is running.
_COMM = "deadbeef-main"

# Field separator for the one-shot query below. It must survive title
# formatting untouched, which rules out more than it looks: tf treats
# '<' and '>' as syntax and silently deletes them, so an obvious-looking
# "<|SEP|>" arrives as "|SEP|" and every field lands in the wrong slot.
# '@' has no meaning in tf, and this run of characters won't occur in a
# path or a tag value by accident.
_SEP = "@@LS@@"

_FIELDS = (
    "%path%",
    "%playback_time_seconds%",
    "%length_seconds%",
    "%ispaused%",
    "%artist%",
    "%title%",
    "%album%",
)

_FORMAT = _SEP.join(_FIELDS)


@dataclass(frozen=True)
class NowPlaying:
    """A single snapshot of what DeaDBeeF is playing."""

    path: str
    position: float  # seconds into the track
    length: float  # seconds, 0.0 if unknown
    paused: bool
    artist: str
    title: str
    album: str

    @property
    def playing(self) -> bool:
        return bool(self.path) and not self.paused


def is_running() -> bool:
    """True if a DeaDBeeF instance is up.

    Reads /proc directly rather than shelling out to pgrep — this is called
    before every poll, and spawning a process to decide whether to spawn a
    process is silly.
    """
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            with open(f"/proc/{entry.name}/comm", "r") as fh:
                if fh.read().strip() == _COMM:
                    return True
        except OSError:
            # Process exited between scandir and open, or we can't read it.
            continue
    return False


def now_playing(timeout: float = 2.0) -> NowPlaying | None:
    """Snapshot the current track, or None if nothing is available.

    Returns None when DeaDBeeF is not running, when it is stopped (no
    current track), or when the CLI misbehaves. Never raises for the
    ordinary "player isn't there" case, because that is a normal state for
    a viewer that outlives the player.
    """
    if not is_running():
        return None

    try:
        completed = subprocess.run(
            ["deadbeef", "--nowplaying-tf", _FORMAT],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return None

    # The "starting deadbeef <version>" banner goes to stderr on every
    # invocation, so stdout is clean and needs no filtering.
    raw = completed.stdout
    if not raw:
        return None

    parts = raw.split(_SEP)
    if len(parts) != len(_FIELDS):
        return None

    path, position, length, paused, artist, title, album = parts

    # Stopped: DeaDBeeF still answers, but with an empty path.
    if not path.strip():
        return None

    return NowPlaying(
        path=path.strip(),
        position=_to_float(position),
        length=_to_float(length),
        paused=paused.strip() == "1",
        artist=artist.strip(),
        title=title.strip(),
        album=album.strip(),
    )


def _to_float(value: str) -> float:
    try:
        return float(value.strip())
    except ValueError:
        return 0.0
