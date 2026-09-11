import pytest
from PySide6.QtCore import QAbstractAnimation, QEventLoop, QTimer
from PySide6.QtWidgets import QApplication, QLabel

from core.lrc import Line
from ui import animations, stylesheet
from ui.lyrics_view import LyricsView


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication([])
    stylesheet.apply_to(instance)
    return instance


@pytest.fixture
def view(app):
    widget = LyricsView()
    widget.resize(560, 700)
    widget.show()
    return widget


def lines(count, tag):
    return [Line(time=float(i), text=f"{tag} line {i}") for i in range(count)]


def settle(ms):
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()


def label_children(view):
    """QLabels parented to the column right now.

    Deferred deletion does not help here: a widget awaiting deleteLater is
    still a child, so this counts the old track's lines too if teardown
    did not reparent them.
    """
    return [child for child in view._body.children() if isinstance(child, QLabel)]


def test_container_holds_only_the_new_track_immediately(view):
    """The count must be right the instant transition_to returns.

    No waiting for the event loop, no waiting for a fade to finish: this is
    what makes fast skipping safe, because the next skip can arrive before
    any deferred cleanup would have run.
    """
    view.set_lines(lines(12, "old"), interactive=True)
    assert len(label_children(view)) == 12

    view.transition_to(lines(7, "new"), interactive=True)

    assert len(view._labels) == 7
    assert len(label_children(view)) == 7, "old labels must be gone synchronously"
    assert all("new" in label.text() for label in view._labels)


def test_rapid_skips_never_accumulate(view):
    """Ten transitions back to back with no event loop in between.

    This is the scenario that stacked: each skip lands while the previous
    one would still have been animating.
    """
    view.set_lines(lines(10, "t0"), interactive=True)

    for i in range(1, 12):
        view.transition_to(lines(10, f"t{i}"), interactive=True)
        assert len(label_children(view)) == 10, f"buildup after skip {i}"

    settle(400)
    assert len(label_children(view)) == 10
    assert all("t11" in label.text() for label in view._labels)


def test_varying_line_counts_stay_exact(view):
    view.set_lines(lines(30, "a"), interactive=True)
    for count in (5, 40, 1, 18):
        view.transition_to(lines(count, "x"), interactive=True)
        assert len(view._labels) == count
        assert len(label_children(view)) == count


def test_transition_fades_the_column_in_once(view):
    """One effect on the container, not one animation per line."""
    view.set_lines(lines(8, "a"), interactive=True)
    view.transition_to(lines(8, "b"), interactive=True)

    assert view._fade is not None
    assert view._fade.state() == QAbstractAnimation.Running
    assert view._body.graphicsEffect() is not None

    settle(400)
    # The effect is removed afterwards rather than left installed.
    assert view._body.graphicsEffect() is None
    assert view._fade is None


def test_interrupted_fade_does_not_leave_an_effect_behind(view):
    view.set_lines(lines(8, "a"), interactive=True)
    view.transition_to(lines(8, "b"), interactive=True)
    view.transition_to(lines(8, "c"), interactive=True)  # cuts the fade off

    settle(400)
    assert view._body.graphicsEffect() is None
    assert len(label_children(view)) == 8


def test_repaint_driver_stops_when_nothing_is_moving(view):
    """The column is transparent, so nothing erases a destroyed label's
    pixels; the driver marks the viewport dirty until things settle."""
    view.set_lines(lines(8, "a"), interactive=True)
    view.transition_to(lines(8, "b"), interactive=True)
    assert view._repaint.isActive()

    settle(500)
    assert not view._repaint.isActive()


def test_empty_lyrics_do_not_start_a_fade(view):
    view.set_lines(lines(6, "a"), interactive=True)
    view.transition_to([], interactive=True)

    assert len(label_children(view)) == 0
    assert view._fade is None


def test_first_highlight_after_a_rebuild_does_not_animate(view):
    """The initial arrangement must be right in the first painted frame.

    With nothing to animate from, tweening would show every line at its
    resting scale and then grow the current one — the pop you see as a
    track's lyrics appear.
    """
    view.set_lines(lines(12, "a"), interactive=True)

    view.set_current(4)  # animate defaults to True

    assert view._scale_animations == [], "first highlight must snap, not tween"
    assert view._labels[4].get_scale() == pytest.approx(animations.CURRENT_SCALE)
    assert view._labels[3].get_scale() == pytest.approx(animations.NORMAL_SCALE)


def test_later_line_changes_do_animate(view):
    view.set_lines(lines(12, "a"), interactive=True)
    view.set_current(4)          # snaps
    view.set_current(5)          # this one should tween

    assert len(view._scale_animations) == 2, "outgoing and incoming both animate"
    assert all(
        a.duration() == animations.LINE_TRANSITION_MS for a in view._scale_animations
    )


def test_scaling_never_reflows_the_column(view):
    """Scale is a paint transform, so geometry must not depend on it."""
    view.set_lines(lines(12, "a"), interactive=True)
    view.set_current(4)
    heights = [label.height() for label in view._labels]
    ys = [label.y() for label in view._labels]

    view.set_current(7, animate=False)

    assert [label.height() for label in view._labels] == heights
    assert [label.y() for label in view._labels] == ys


def test_unsynced_lines_sit_at_full_scale(view):
    """Nothing is focused, so nothing should be shrunk."""
    view.set_lines(lines(5, "plain"), interactive=False)
    assert all(
        label.get_scale() == pytest.approx(animations.CURRENT_SCALE)
        for label in view._labels
    )
