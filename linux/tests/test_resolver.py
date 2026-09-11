import os

from core import resolver

SYNCED = "[00:03.00] stamped line\n[00:06.00] another\n"
PLAIN = "just words\nmore words\n"


def _track(tmp_path, name="song.flac"):
    path = tmp_path / name
    path.write_bytes(b"not really audio")
    return str(path)


def test_sidecar_lrc_next_to_track(tmp_path):
    track = _track(tmp_path)
    (tmp_path / "song.lrc").write_text(SYNCED)

    found = resolver.resolve(track, library=str(tmp_path / "nolib"))
    assert found.lyrics.synced
    assert "sidecar" in found.source


def test_sidecar_txt_is_used_when_no_lrc(tmp_path):
    track = _track(tmp_path)
    (tmp_path / "song.txt").write_text(PLAIN)

    found = resolver.resolve(track, library=str(tmp_path / "nolib"))
    assert found
    assert not found.lyrics.synced


def test_library_lookup_by_artist_and_title(tmp_path):
    library = tmp_path / "lib"
    library.mkdir()
    (library / "Avicii - The Nights.lrc").write_text(SYNCED)

    found = resolver.resolve(_track(tmp_path), "Avicii", "The Nights", library=str(library))
    assert found.lyrics.synced
    assert "library" in found.source


def test_library_falls_back_to_title_only(tmp_path):
    # The file credits several artists but the track's tag names one.
    library = tmp_path / "lib"
    library.mkdir()
    (library / "Avicii, Aloe Blacc - SOS.lrc").write_text(SYNCED)

    found = resolver.resolve(_track(tmp_path), "Avicii", "SOS", library=str(library))
    assert found.lyrics.synced


def test_library_match_ignores_case_and_lyrics_suffix(tmp_path):
    library = tmp_path / "lib"
    library.mkdir()
    (library / "Imagine Dragons - Demons (Lyric Video).lrc").write_text(SYNCED)

    found = resolver.resolve(
        _track(tmp_path), "Imagine Dragons", "demons", library=str(library)
    )
    assert found.lyrics.synced


def test_synced_source_wins_over_unsynced_one(tmp_path):
    # Plain text sits right next to the track; the synced copy is in the
    # library. The synced one should win despite being lower priority.
    track = _track(tmp_path)
    (tmp_path / "song.txt").write_text(PLAIN)
    library = tmp_path / "lib"
    library.mkdir()
    (library / "Avicii - The Nights.lrc").write_text(SYNCED)

    found = resolver.resolve(track, "Avicii", "The Nights", library=str(library))
    assert found.lyrics.synced
    assert "library" in found.source


def test_nothing_found(tmp_path):
    found = resolver.resolve(_track(tmp_path), "Nobody", "Nothing", library=str(tmp_path / "nolib"))
    assert not found
    assert found.source == ""


def test_missing_track_path_is_survivable():
    found = resolver.resolve("", "", "", library="/nonexistent")
    assert not found
