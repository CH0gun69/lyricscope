"""Animation tuning in one place.

Colours stay in styles.qss, but sizes live here rather than there: a Qt
stylesheet's `font-size` overrides anything set with setFont(), so a line
whose size is declared in QSS cannot be animated at all. The scale
animation only works because the lyric line sizes are driven from Python.

Everything is deliberately short. Poweramp's line change reads as a pop
with easing rather than a tween, and a stagger only looks like a cascade
while it is quicker than the eye tracks it.
"""

from __future__ import annotations

from PySide6.QtCore import QEasingCurve

# -- line sizing ---------------------------------------------------------

# Pixel size of a line at full scale. Non-current lines sit at NORMAL_SCALE
# of this, so the two states are 26px and ~21px.
LINE_PX = 26
NORMAL_SCALE = 0.80
CURRENT_SCALE = 1.0

# -- current-line transition (feature 1) ---------------------------------

LINE_TRANSITION_MS = 190
LINE_TRANSITION_EASING = QEasingCurve.OutCubic

# How long the view takes to re-centre on the new current line. Slightly
# longer than the scale change so the scroll settles after the pop rather
# than racing it.
SCROLL_MS = 420
SCROLL_EASING = QEasingCurve.OutCubic

# -- track change --------------------------------------------------------

# The old lines are destroyed outright and the new ones fade in as one
# block. This replaced a staggered per-line slide cascade: that version
# looked good but decided when to destroy widgets from per-line
# animation-finished callbacks, and anything that interrupted those — a
# fast skip, a cancelled animation — risked leaving labels behind.
#
# Short enough to read as a swap rather than a transition. Much past
# ~200ms it starts to feel like the app is thinking.
LYRICS_FADE_MS = 130
LYRICS_FADE_EASING = QEasingCurve.OutCubic

# -- backdrop (features 3 and 4a) ----------------------------------------

BACKDROP_FADE_MS = 620
BACKDROP_FADE_EASING = QEasingCurve.InOutQuad

# The blur is built once per track. Downscaling before blurring is most of
# the smoothing (and most of the speed); the gaussian just removes the
# hard edges left by the resample.
BLUR_DOWNSCALE = 6
BLUR_RADIUS = 12

# Darkening applied over the art so lyrics stay readable on bright covers.
# At 0.62 the art was technically present but read as flat black; this is
# the point where the colour is obvious without the dim lines washing out.
BACKDROP_SCRIM_ALPHA = 0.45
