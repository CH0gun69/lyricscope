"""Seek the running DeaDBeeF instance.

DeaDBeeF's CLI has no seek flag — `--help` offers play/pause/next/prev and
volume, nothing positional — so seeking has to go through MPRIS, which its
bundled mpris plugin implements properly (both SetPosition and Seek were
verified against a live instance).

This shells out to `gdbus` rather than using PySide6's QtDBus, which cannot
express either call:

  * `Seek` takes an int64 (`x`). PySide6 marshals a Python int as int32
    unless its value happens to exceed the int32 range, so every realistic
    offset is rejected with "Type of message, (i), does not match expected
    type (x)". Only absurd offsets get through, which is worse than
    failing — one such test seek ran off the end of the track.
  * `SetPosition` would sidestep that by taking the track id, but reading
    Metadata (a{sv}) needs QDBusArgument demarshalling, and PySide6's
    asVariant() returns None there.

A seek is user-initiated and rare, so the ~5ms subprocess is not worth
fighting the marshalling for.
"""

from __future__ import annotations

import subprocess

SERVICE = "org.mpris.MediaPlayer2.DeaDBeeF"  # capitalised; the lowercase
PATH = "/org/mpris/MediaPlayer2"             # guess gets ServiceUnknown
PLAYER = "org.mpris.MediaPlayer2.Player"


def available() -> bool:
    """True if DeaDBeeF is on the session bus and can be seeked."""
    return _get("CanSeek") == "true"


def position() -> float | None:
    """Playback position in seconds, straight from MPRIS.

    Used to anchor a relative seek. The viewer's own interpolated position
    is close but can be up to a poll interval stale, and that error would
    land the seek on the wrong lyric line.
    """
    raw = _get("Position")
    if raw is None:
        return None
    try:
        return int(raw) / 1e6
    except ValueError:
        return None


def to_seconds(target: float, timeout: float = 2.0) -> bool:
    """Seek to an absolute position, expressed as a relative Seek."""
    current = position()
    if current is None:
        return False

    offset = int(round((target - current) * 1e6))
    return _call(PLAYER + ".Seek", str(offset), timeout=timeout)


def _get(name: str, timeout: float = 2.0) -> str | None:
    out = _run(
        ["--method", "org.freedesktop.DBus.Properties.Get", PLAYER, name],
        timeout,
    )
    if out is None:
        return None
    # Replies look like "(<int64 33719100>,)" or "(<true>,)".
    body = out.strip().lstrip("(<").rstrip(">,)").strip()
    if body.startswith("int64"):
        body = body[len("int64"):].strip()
    return body or None


def _call(method: str, *args: str, timeout: float = 2.0) -> bool:
    return _run(["--method", method, *args], timeout) is not None


def _run(args: list[str], timeout: float) -> str | None:
    try:
        completed = subprocess.run(
            # The "--" matters: a backward seek's offset is negative, and
            # without the terminator gdbus reads "-73380000" as an option
            # and prints its usage instead. Seeking forward would work and
            # seeking back would silently do nothing.
            ["gdbus", "call", "--session", "--dest", SERVICE,
             "--object-path", PATH, *args[:2], "--", *args[2:]],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout
