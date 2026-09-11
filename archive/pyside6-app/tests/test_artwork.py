import io

import pytest

from core import artwork

PIL = pytest.importorskip("PIL")
from PIL import Image  # noqa: E402


def _png(width, height, colour=(120, 40, 200)):
    buf = io.BytesIO()
    Image.new("RGB", (width, height), colour).save(buf, format="PNG")
    return buf.getvalue()


def _size(data):
    return Image.open(io.BytesIO(data)).size


def test_blur_fills_the_requested_size():
    out = artwork.blur(_png(1280, 720), (560, 780), downscale=6, radius=12)
    assert _size(out) == (560, 780)


def test_blur_crops_rather_than_stretching():
    # A 16:9 cover in a portrait window: cropping keeps circles round,
    # stretching does not. The crop is centred, so a frame that is red on
    # the left third stays red-free in the middle.
    image = Image.new("RGB", (900, 300), (0, 0, 0))
    for x in range(300):
        for y in range(300):
            image.putpixel((x, y), (255, 0, 0))
    buf = io.BytesIO(); image.save(buf, format="PNG")

    out = artwork.blur(buf.getvalue(), (300, 300), downscale=1, radius=0)
    result = Image.open(io.BytesIO(out))
    assert result.size == (300, 300)
    # Centre pixel comes from the middle third, which was black.
    r, g, b = result.getpixel((150, 150))
    assert r < 60, "centre should come from the middle of the source, not the red edge"


def test_blur_actually_softens():
    # Half black, half white: after blurring, the seam is no longer a
    # hard step, so pixels near it are mid-grey.
    image = Image.new("RGB", (200, 200), (0, 0, 0))
    for x in range(100, 200):
        for y in range(200):
            image.putpixel((x, y), (255, 255, 255))
    buf = io.BytesIO(); image.save(buf, format="PNG")

    out = artwork.blur(buf.getvalue(), (200, 200), downscale=2, radius=10)
    result = Image.open(io.BytesIO(out)).convert("RGB")
    r, _, _ = result.getpixel((100, 100))
    assert 40 < r < 215, f"seam should be blurred, got {r}"


def test_blur_of_nothing_is_empty():
    assert artwork.blur(b"", (560, 780), 6, 12) == b""


def test_blur_rejects_a_zero_sized_window():
    assert artwork.blur(_png(100, 100), (0, 780), 6, 12) == b""


def test_load_finds_a_cover_beside_the_track(tmp_path):
    (tmp_path / "song.flac").write_bytes(b"not audio")
    (tmp_path / "cover.jpg").write_bytes(_png(50, 50))

    found = artwork.load(str(tmp_path / "song.flac"), cache_dir=str(tmp_path / "none"))
    assert found
    assert "folder" in found.source


def test_load_returns_empty_when_there_is_no_art(tmp_path):
    (tmp_path / "song.flac").write_bytes(b"not audio")
    found = artwork.load(str(tmp_path / "song.flac"), cache_dir=str(tmp_path / "none"))
    assert not found
    assert found.data == b""


def test_load_survives_a_missing_track():
    assert not artwork.load("/nonexistent/song.flac", cache_dir="/nonexistent")
