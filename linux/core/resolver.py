"""Find the lyrics for a track.

Three sources are tried, mirroring what deadbeef-lyricbar does locally
(minus its online fetchers, which this viewer deliberately does not have):

  1. a sidecar file next to the audio — <track>.lrc, then <track>.txt
  2. lyrics embedded in the file's own tags
  3. a central lyrics folder keyed by "<artist> - <title>"

(3) is not something lyricbar does, but it is how foobar2000's
foo_openlyrics stores its library, so an imported collection lands there
rather than beside the tracks. On this machine that is the only populated
source.

Rather than taking the first source that answers, all three are consulted
and a *synced* result wins over an unsynced one. Otherwise a stale plain
text lyrics tag would mask a properly timestamped .lrc sitting right next
to the track.
"""

from __future__ import annotations

import os
import re
import unicodedata
from dataclasses import dataclass
from functools import lru_cache

from core import lrc

DEFAULT_LIBRARY = os.path.expanduser("~/.config/deadbeef/lyrics")

_SIDECAR_SUFFIXES = (".lrc", ".txt")

# Tag names holding synced lyrics, then unsynced. Order and spelling follow
# lyricbar's get_lyrics_from_metadata(); Vorbis comments are case-insensitive
# in practice, so lookups are normalised to lowercase.
_SYNCED_KEYS = ("lyrics", "sylt", "syncedlyrics")
_UNSYNCED_KEYS = ("unsyncedlyrics", "unsynced lyrics", "uslt", "©lyr", "lyrics:description")

# Trailing junk on filenames imported from YouTube-sourced lyrics, e.g.
# "Demons (Lyric Video)" or "Assumptions (Lyrics)".
_NOISE = re.compile(r"\s*[\(\[](?:official\s+)?(?:lyrics?|lyric\s+video|audio|video)[^\)\]]*[\)\]]\s*", re.I)
_NON_ALNUM = re.compile(r"[^a-z0-9]+")


@dataclass(frozen=True)
class Resolved:
    lyrics: lrc.Lyrics
    source: str  # human-readable, shown in the UI status line

    def __bool__(self) -> bool:
        return bool(self.lyrics)


def resolve(path: str, artist: str = "", title: str = "",
            library: str = DEFAULT_LIBRARY) -> Resolved:
    """Resolve lyrics for a track, preferring synced over unsynced."""
    candidates: list[Resolved] = []

    for finder in (
        lambda: _from_sidecar(path),
        lambda: _from_tags(path),
        lambda: _from_library(library, artist, title),
    ):
        found = finder()
        if found:
            if found.lyrics.synced:
                return found
            candidates.append(found)

    return candidates[0] if candidates else Resolved(lrc.Lyrics(), "")


def _from_sidecar(path: str) -> Resolved | None:
    if not path:
        return None
    base, _ = os.path.splitext(path)
    for suffix in _SIDECAR_SUFFIXES:
        candidate = base + suffix
        text = _read(candidate)
        if text:
            return Resolved(lrc.parse(text), f"sidecar: {os.path.basename(candidate)}")
    return None


def _from_tags(path: str) -> Resolved | None:
    if not path or not os.path.isfile(path):
        return None
    try:
        import mutagen
    except ImportError:
        return None

    try:
        audio = mutagen.File(path)
    except Exception:
        # Unsupported container or unreadable file — not fatal, just means
        # this source has nothing to offer.
        return None
    if audio is None or not getattr(audio, "tags", None):
        return None

    flat = {}
    try:
        for key, value in audio.tags.items():
            flat[str(key).lower()] = value
    except Exception:
        return None

    for keys in (_SYNCED_KEYS, _UNSYNCED_KEYS):
        for key in keys:
            for tag_key, value in flat.items():
                # ID3 frames are keyed like "USLT::eng", so match on prefix.
                if tag_key != key and not tag_key.startswith(key):
                    continue
                text = _unescape(_stringify(value))
                if text and text.strip():
                    return Resolved(lrc.parse(text), "embedded tag")
    return None


def _unescape(text: str) -> str:
    r"""Undo JSON escaping that leaks into embedded lyrics tags.

    Lyrics fetched from web APIs and written back into tags often keep the
    transport's escaping, so lines arrive as \"These are the nights\".
    Only quote escapes are undone — a lone backslash is left alone, since
    it is far likelier to be real text than a broken escape.
    """
    return text.replace('\\"', '"').replace("\\'", "'")


def _from_library(library: str, artist: str, title: str) -> Resolved | None:
    if not library or not title or not os.path.isdir(library):
        return None

    for stem in (f"{artist} - {title}" if artist else "", title):
        if not stem:
            continue
        for suffix in _SIDECAR_SUFFIXES:
            text = _read(os.path.join(library, stem + suffix))
            if text:
                return Resolved(lrc.parse(text), f"library: {stem}{suffix}")

    # Nothing matched literally — fall back to a normalised comparison so
    # that punctuation, case and "(Lyrics)" suffixes stop mattering.
    index = _library_index(library, _stamp(library))
    for key in (_normalise(f"{artist} - {title}") if artist else "", _normalise(title)):
        if key and key in index:
            name = index[key]
            text = _read(os.path.join(library, name))
            if text:
                return Resolved(lrc.parse(text), f"library: {name}")
    return None


def _stamp(library: str) -> float:
    """Directory mtime, so the cached index rebuilds when files are added."""
    try:
        return os.stat(library).st_mtime
    except OSError:
        return 0.0


@lru_cache(maxsize=4)
def _library_index(library: str, _stamp: float) -> dict[str, str]:
    """Map normalised "artist - title" and bare title to a filename."""
    index: dict[str, str] = {}
    try:
        names = os.listdir(library)
    except OSError:
        return index

    for name in names:
        stem, suffix = os.path.splitext(name)
        if suffix.lower() not in _SIDECAR_SUFFIXES:
            continue
        full = _normalise(stem)
        if full:
            index.setdefault(full, name)
        # Also key on the part after "artist - ", so a track whose tags name
        # only one of several credited artists still matches.
        if " - " in stem:
            bare = _normalise(stem.split(" - ", 1)[1])
            if bare:
                index.setdefault(bare, name)
    return index


def _normalise(value: str) -> str:
    value = _NOISE.sub(" ", value or "")
    value = unicodedata.normalize("NFKD", value)
    value = "".join(ch for ch in value if not unicodedata.combining(ch))
    return _NON_ALNUM.sub(" ", value.lower()).strip()


def _stringify(value) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        return "\n".join(_stringify(v) for v in value)
    # mutagen frame objects (USLT, COMM) expose the payload as .text
    text = getattr(value, "text", None)
    if text is not None:
        return _stringify(text)
    return str(value)


def _read(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""
