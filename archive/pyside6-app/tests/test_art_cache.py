import io

import pytest

pytest.importorskip("PIL")
from PIL import Image  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from core import artwork  # noqa: E402


@pytest.fixture(scope="module")
def app():
    return QApplication.instance() or QApplication([])


def _png(colour=(10, 120, 200)):
    buf = io.BytesIO()
    Image.new("RGB", (400, 400), colour).save(buf, format="PNG")
    return buf.getvalue()


@pytest.fixture
def window(app, monkeypatch):
    # Keep the window from talking to DeaDBeeF while it is being built.
    from core import deadbeef

    monkeypatch.setattr(deadbeef, "is_running", lambda: False)
    monkeypatch.setattr(deadbeef, "now_playing", lambda timeout=2.0: None)

    from ui.main_window import MainWindow

    return MainWindow()


def test_blur_is_computed_once_per_track(window, monkeypatch):
    calls = []

    monkeypatch.setattr(artwork, "load", lambda p: artwork.Artwork(_png(), "test"))

    real_blur = artwork.blur

    def counted(data, size, downscale, radius):
        calls.append(size)
        return real_blur(data, size, downscale, radius)

    monkeypatch.setattr(artwork, "blur", counted)

    first = window._artwork_for("/music/a.flac")
    second = window._artwork_for("/music/a.flac")
    third = window._artwork_for("/music/a.flac")

    assert len(calls) == 1, "a track's backdrop must be blurred once, not per frame"
    assert not first.isNull()
    # The same cached object is handed back, not a re-decode.
    assert second is first and third is first


def test_each_track_gets_its_own_backdrop(window, monkeypatch):
    calls = []
    monkeypatch.setattr(artwork, "load", lambda p: artwork.Artwork(_png(), "test"))

    real_blur = artwork.blur

    def counted(data, size, downscale, radius):
        calls.append(size)
        return real_blur(data, size, downscale, radius)

    monkeypatch.setattr(artwork, "blur", counted)

    window._artwork_for("/music/a.flac")
    window._artwork_for("/music/b.flac")
    assert len(calls) == 2


def test_a_track_without_art_is_not_reprobed(window, monkeypatch):
    probes = []

    def no_art(path):
        probes.append(path)
        return artwork.Artwork(b"", "")

    monkeypatch.setattr(artwork, "load", no_art)

    assert window._artwork_for("/music/none.flac").isNull()
    assert window._artwork_for("/music/none.flac").isNull()
    assert len(probes) == 1, "an absent cover should be cached too"
