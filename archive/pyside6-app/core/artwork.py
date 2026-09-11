"""Find a track's cover art and pre-blur it for use as a backdrop.

Art is looked for in the track's own tags first, then next to it on disk,
then in DeaDBeeF's cover cache. The cache is worth checking last rather
than first: DeaDBeeF names those files after the album, so several tracks
share one, and it only holds covers it has already fetched.

The blur is built once per track and cached. Doing it per frame is what
makes this kind of backdrop crawl — a blur costs ~30ms, which is two
dropped frames every frame.

Blurring is done with Pillow rather than QGraphicsBlurEffect. Both measure
the same (~29ms), but the Qt route has to go through a QGraphicsScene to
produce a pixmap and its effect bleeds the scene background in at the
edges; Pillow's output is predictable and the downscale/upscale does most
of the smoothing for free.
"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass

# Cover files sitting beside the audio, in the order DeaDBeeF itself
# prefers them.
_FOLDER_NAMES = ("cover", "folder", "front", "albumart", "album")
_IMAGE_SUFFIXES = (".jpg", ".jpeg", ".png", ".webp")

DEADBEEF_COVER_CACHE = os.path.expanduser("~/.cache/deadbeef/covers2")


@dataclass(frozen=True)
class Artwork:
    data: bytes
    source: str

    def __bool__(self) -> bool:
        return bool(self.data)


def load(path: str, cache_dir: str = DEADBEEF_COVER_CACHE) -> Artwork:
    """Return raw image bytes for a track's cover, or an empty Artwork."""
    for finder in (_embedded, _beside, lambda p: _from_cache(p, cache_dir)):
        try:
            found = finder(path)
        except Exception:
            # A malformed tag or unreadable file shouldn't cost us the
            # backdrop, let alone the window.
            continue
        if found:
            return found
    return Artwork(b"", "")


def _embedded(path: str) -> Artwork | None:
    if not path or not os.path.isfile(path):
        return None
    try:
        import mutagen
    except ImportError:
        return None

    audio = mutagen.File(path)
    if audio is None:
        return None

    # FLAC / Ogg expose .pictures; ID3 hides art in APIC frames; MP4 in covr.
    for picture in getattr(audio, "pictures", None) or []:
        if picture.data:
            return Artwork(bytes(picture.data), "embedded")

    tags = getattr(audio, "tags", None)
    if not tags:
        return None

    for key in getattr(tags, "keys", lambda: [])():
        name = str(key)
        if name.startswith("APIC"):
            frame = tags[key]
            data = getattr(frame, "data", None)
            if data:
                return Artwork(bytes(data), "embedded")
        if name == "covr":
            covers = tags[key]
            if covers:
                return Artwork(bytes(covers[0]), "embedded")
    return None


def _beside(path: str) -> Artwork | None:
    if not path:
        return None
    folder = os.path.dirname(path)
    if not os.path.isdir(folder):
        return None

    for stem in _FOLDER_NAMES:
        for suffix in _IMAGE_SUFFIXES:
            for candidate in (stem + suffix, stem.capitalize() + suffix):
                full = os.path.join(folder, candidate)
                if os.path.isfile(full):
                    with open(full, "rb") as fh:
                        return Artwork(fh.read(), f"folder: {candidate}")
    return None


def _from_cache(path: str, cache_dir: str) -> Artwork | None:
    """DeaDBeeF's own cover cache, keyed loosely by the track's folder."""
    if not path or not os.path.isdir(cache_dir):
        return None

    # Names look like "SHARED-Library-Album - Artist.jpg" — the track's
    # parent folders appear in the key, so match on those.
    parts = [p for p in path.split(os.sep) if p][:-1][-2:]
    if not parts:
        return None
    needle = "-".join(parts).lower()

    try:
        names = sorted(os.listdir(cache_dir))
    except OSError:
        return None

    for name in names:
        if name.lower().startswith(needle):
            full = os.path.join(cache_dir, name)
            try:
                with open(full, "rb") as fh:
                    return Artwork(fh.read(), f"deadbeef cache: {name}")
            except OSError:
                continue
    return None


def blur(data: bytes, size: tuple[int, int], downscale: int, radius: int) -> bytes:
    """Blur cover art to fill `size`, returned as PNG bytes.

    Bytes rather than a QPixmap so this stays usable off the GUI thread and
    testable without a QApplication.
    """
    from PIL import Image, ImageFilter

    width, height = size
    if width <= 0 or height <= 0 or not data:
        return b""

    image = Image.open(io.BytesIO(data)).convert("RGB")
    image = _cover_crop(image, width / height)

    small = image.resize(
        (max(1, image.width // downscale), max(1, image.height // downscale)),
        Image.LANCZOS,
    )
    small = small.filter(ImageFilter.GaussianBlur(radius=radius))

    out = io.BytesIO()
    small.resize((width, height), Image.LANCZOS).save(out, format="PNG")
    return out.getvalue()


def _cover_crop(image, aspect: float):
    """Centre-crop to `aspect` so filling the window doesn't stretch the art.

    Covers are square or 16:9 and the window is portrait, so without this
    the backdrop is visibly distorted.
    """
    if image.width <= 0 or image.height <= 0:
        return image

    current = image.width / image.height
    if abs(current - aspect) < 1e-3:
        return image

    if current > aspect:
        new_width = max(1, int(image.height * aspect))
        left = (image.width - new_width) // 2
        return image.crop((left, 0, left + new_width, image.height))

    new_height = max(1, int(image.width / aspect))
    top = (image.height - new_height) // 2
    return image.crop((0, top, image.width, top + new_height))
