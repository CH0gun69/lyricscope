import subprocess
from types import SimpleNamespace

import pytest

from core import seek


def _capture(calls, stdout="()", returncode=0):
    def run(cmd, **kwargs):
        calls.append(cmd)
        return SimpleNamespace(stdout=stdout, stderr="", returncode=returncode)
    return run


def test_negative_offsets_are_passed_after_a_terminator(monkeypatch):
    # gdbus reads a bare "-73380000" as a command-line option and prints
    # usage instead of seeking, so backward seeks would silently no-op.
    calls = []
    monkeypatch.setattr(seek, "position", lambda: 100.0)
    monkeypatch.setattr(subprocess, "run", _capture(calls))

    assert seek.to_seconds(30.0) is True

    cmd = calls[-1]
    assert "--" in cmd, "arguments must be separated from options"
    offset = cmd[-1]
    assert offset.startswith("-"), "seeking backwards means a negative offset"
    assert cmd.index("--") < cmd.index(offset)


def test_absolute_target_becomes_a_relative_offset(monkeypatch):
    calls = []
    monkeypatch.setattr(seek, "position", lambda: 40.0)
    monkeypatch.setattr(subprocess, "run", _capture(calls))

    seek.to_seconds(95.5)
    # (95.5 - 40.0) seconds, in microseconds.
    assert calls[-1][-1] == str(int(55.5 * 1e6))


def test_seek_uses_the_players_own_position_not_an_estimate(monkeypatch):
    # The viewer's interpolated position can be up to a poll interval
    # stale; anchoring on it would land the seek on the wrong line.
    probed = []

    def fake_position():
        probed.append(True)
        return 10.0

    monkeypatch.setattr(seek, "position", fake_position)
    monkeypatch.setattr(subprocess, "run", _capture([]))
    seek.to_seconds(20.0)
    assert probed, "to_seconds must read the live position first"


def test_seek_fails_cleanly_when_position_is_unavailable(monkeypatch):
    monkeypatch.setattr(seek, "position", lambda: None)

    def explode(*a, **k):
        raise AssertionError("must not seek without a reference position")

    monkeypatch.setattr(subprocess, "run", explode)
    assert seek.to_seconds(30.0) is False


def test_position_parses_the_gdbus_reply(monkeypatch):
    monkeypatch.setattr(subprocess, "run", _capture([], stdout="(<int64 33719100>,)\n"))
    assert seek.position() == pytest.approx(33.7191)


def test_available_reads_can_seek(monkeypatch):
    monkeypatch.setattr(subprocess, "run", _capture([], stdout="(<true>,)\n"))
    assert seek.available() is True

    monkeypatch.setattr(subprocess, "run", _capture([], stdout="(<false>,)\n"))
    assert seek.available() is False


def test_player_absent_is_not_fatal(monkeypatch):
    monkeypatch.setattr(subprocess, "run", _capture([], stdout="", returncode=1))
    assert seek.position() is None
    assert seek.available() is False


def test_gdbus_missing_is_not_fatal(monkeypatch):
    def missing(*a, **k):
        raise FileNotFoundError("gdbus")

    monkeypatch.setattr(subprocess, "run", missing)
    assert seek.position() is None
