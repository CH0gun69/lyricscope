from core.deadbeef import NowPlaying
from core.tracker import Tracker


def _state(position=10.0, length=100.0, paused=False, path="/music/a.flac"):
    return NowPlaying(
        path=path, position=position, length=length,
        paused=paused, artist="A", title="T", album="",
    )


def test_position_advances_between_polls():
    t = Tracker()
    t.state = _state(position=10.0)
    t._polled_at = 100.0
    # Two seconds of wall clock since the poll that reported 10.0s.
    assert t.position(now=102.0) == 10.0 + 2.0


def test_paused_position_does_not_drift():
    t = Tracker()
    t.state = _state(position=10.0, paused=True)
    t._polled_at = 100.0
    assert t.position(now=130.0) == 10.0


def test_interpolation_stops_at_track_end():
    # Otherwise the highlight would run past the last line while waiting
    # for the poll that reports the next track.
    t = Tracker()
    t.state = _state(position=95.0, length=100.0)
    t._polled_at = 100.0
    assert t.position(now=200.0) == 100.0


def test_no_state_means_zero():
    assert Tracker().position(now=123.0) == 0.0


def test_due_respects_the_interval():
    t = Tracker(poll_interval=0.5)
    t._last_poll = 100.0
    assert not t.due(now=100.4)
    assert t.due(now=100.5)


def test_poll_reports_track_changes(monkeypatch):
    import core.tracker as tracker_mod

    t = Tracker()
    monkeypatch.setattr(tracker_mod, "now_playing", lambda: _state(path="/music/a.flac"))
    assert t.poll() is True  # nothing -> a.flac

    monkeypatch.setattr(tracker_mod, "now_playing", lambda: _state(path="/music/a.flac"))
    assert t.poll() is False  # same track

    monkeypatch.setattr(tracker_mod, "now_playing", lambda: _state(path="/music/b.flac"))
    assert t.poll() is True  # a.flac -> b.flac
