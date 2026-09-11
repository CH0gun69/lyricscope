import pytest
from PySide6.QtCore import QEvent, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtWidgets import QApplication

from ui import animations
from ui.lyric_label import LyricLabel


@pytest.fixture(scope="module")
def app():
    return QApplication.instance() or QApplication([])


def _press(label, button=Qt.LeftButton):
    local = QPointF(label.width() / 2, label.height() / 2)
    event = QMouseEvent(
        QEvent.MouseButtonPress,
        local,
        label.mapToGlobal(local.toPoint()).toPointF(),
        button, button, Qt.NoModifier,
    )
    QApplication.sendEvent(label, event)
    return event


def test_seekable_line_accepts_the_press(app):
    """Accepting the press is what keeps click-to-seek working.

    QLabel ignores mouse events unless text interaction is enabled. An
    ignored press propagates to the parent and takes the implicit mouse
    grab with it, so the release never reaches this widget and the click is
    lost. Removing the press handler as "dead code" silently breaks
    click-to-seek, which no click-simulating test would catch, because
    sending events directly to a widget bypasses grab semantics entirely.
    """
    label = LyricLabel("a line")
    label.resize(300, 30)
    label.set_seekable(True)
    label.show()

    assert _press(label).isAccepted()


def test_non_seekable_line_does_not_swallow_the_press(app):
    """Unsynced lines have nothing to seek to, so the press should pass on."""
    label = LyricLabel("a line")
    label.resize(300, 30)
    label.set_seekable(False)
    label.show()

    assert not _press(label).isAccepted()


def test_right_click_is_left_alone(app):
    label = LyricLabel("a line")
    label.resize(300, 30)
    label.set_seekable(True)
    label.show()

    assert not _press(label, Qt.RightButton).isAccepted()


def test_font_is_fixed_at_full_size_so_scaling_cannot_reflow(app):
    """Scale is a paint transform; the font must not track it."""
    label = LyricLabel("a line")
    label.show()
    before = label.font().pixelSize()

    label.set_scale(animations.NORMAL_SCALE)
    assert label.font().pixelSize() == before
    label.set_scale(animations.CURRENT_SCALE)
    assert label.font().pixelSize() == before == animations.LINE_PX


def test_scale_is_continuous_not_quantised(app):
    """The whole point of the transform: sizes between the two rest states.

    A font-based implementation could only produce six distinct sizes
    between 0.80 and 1.00 of 26px.
    """
    label = LyricLabel("a line")
    label.show()
    seen = set()
    for i in range(41):
        value = animations.NORMAL_SCALE + (
            animations.CURRENT_SCALE - animations.NORMAL_SCALE
        ) * i / 40
        label.set_scale(value)
        seen.add(round(label.get_scale(), 4))
    assert len(seen) > 30, f"expected continuous scaling, got {len(seen)} steps"
