import subprocess
from types import SimpleNamespace

import pytest

from core import deadbeef


def _stub_run(stdout: str):
    def run(cmd, **kwargs):
        return SimpleNamespace(stdout=stdout, stderr="", returncode=0)
    return run


@pytest.fixture
def player_up(monkeypatch):
    monkeypatch.setattr(deadbeef, "is_running", lambda: True)


def _line(*fields):
    return deadbeef._SEP.join(fields)


def test_separator_survives_title_formatting():
    # DeaDBeeF's title formatting deletes '<' and '>' as syntax, which
    # silently shifts every field if the separator contains them.
    assert "<" not in deadbeef._SEP
    assert ">" not in deadbeef._SEP


def test_parses_a_snapshot(player_up, monkeypatch):
    monkeypatch.setattr(subprocess, "run", _stub_run(
        _line("/music/The Nights.flac", "49.53", "177", "0", "Avicii", "The Nights", "The Days / Nights")
    ))
    np = deadbeef.now_playing()
    assert np.path == "/music/The Nights.flac"
    assert np.position == 49.53
    assert np.length == 177.0
    assert np.paused is False
    assert np.playing is True
    assert np.artist == "Avicii"


def test_paused_is_not_playing(player_up, monkeypatch):
    monkeypatch.setattr(subprocess, "run", _stub_run(
        _line("/music/a.flac", "1.0", "10", "1", "A", "T", "")
    ))
    np = deadbeef.now_playing()
    assert np.paused is True
    assert np.playing is False


def test_path_with_separator_like_characters(player_up, monkeypatch):
    # A path containing '|' or '@' must not be mistaken for a field break.
    monkeypatch.setattr(subprocess, "run", _stub_run(
        _line("/music/AC|DC @ Live.flac", "2.0", "10", "0", "AC|DC", "Live @ Home", "")
    ))
    np = deadbeef.now_playing()
    assert np.path == "/music/AC|DC @ Live.flac"
    assert np.title == "Live @ Home"


def test_stopped_player_reports_nothing(player_up, monkeypatch):
    # Stopped: DeaDBeeF answers, but with an empty path.
    monkeypatch.setattr(subprocess, "run", _stub_run(_line("", "0", "0", "0", "", "", "")))
    assert deadbeef.now_playing() is None


def test_malformed_output_is_not_fatal(player_up, monkeypatch):
    monkeypatch.setattr(subprocess, "run", _stub_run("nonsense without separators"))
    assert deadbeef.now_playing() is None


def test_never_invokes_cli_when_player_is_down(monkeypatch):
    # Calling the CLI with no instance reachable makes it start the player,
    # so a poll must not happen at all in that state.
    monkeypatch.setattr(deadbeef, "is_running", lambda: False)

    def explode(*a, **k):
        raise AssertionError("the CLI must not be invoked while DeaDBeeF is down")

    monkeypatch.setattr(subprocess, "run", explode)
    assert deadbeef.now_playing() is None


def test_timeout_is_not_fatal(player_up, monkeypatch):
    def timeout(*a, **k):
        raise subprocess.TimeoutExpired(cmd="deadbeef", timeout=2.0)

    monkeypatch.setattr(subprocess, "run", timeout)
    assert deadbeef.now_playing() is None
