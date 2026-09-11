/* Every duration, easing and size in one place — the C counterpart of the
 * PySide6 app's ui/animations.py, deliberately carrying the same values so
 * the port feels identical rather than merely similar.
 *
 * The reason this is a header of constants rather than, say, GTK theme
 * properties is the same reason it was a Python module rather than QSS:
 * the line scale has to be a number this code interpolates, and anything
 * that lets the toolkit own the text size takes that away.
 */

#ifndef LS_ANIMATIONS_H
#define LS_ANIMATIONS_H

/* -- line sizing ------------------------------------------------------ */

/* Pixel size of a line at full scale. Non-current lines sit at
 * LS_NORMAL_SCALE of this, so the two states are 26px and ~21px. */
#define LS_LINE_PX 26.0
#define LS_NORMAL_SCALE 0.80
#define LS_CURRENT_SCALE 1.0

/* Gap between lines, and how far down the panel the current line sits. */
#define LS_LINE_SPACING 16.0
#define LS_SIDE_PADDING 48.0
#define LS_FOCUS 0.42

/* -- current-line transition ------------------------------------------ */

#define LS_LINE_TRANSITION_MS 190.0

/* Slightly longer than the scale change, so the scroll settles after the
 * pop rather than racing it. */
#define LS_SCROLL_MS 420.0

/* -- track change ----------------------------------------------------- */

/* Short enough to read as a swap rather than a transition. Much past
 * ~200ms it starts to feel like the panel is thinking. */
#define LS_LYRICS_FADE_MS 130.0

/* -- backdrop --------------------------------------------------------- */

#define LS_BACKDROP_FADE_MS 620.0

/* The blur is built once per track. Downscaling before blurring is most of
 * the smoothing and most of the speed; the blur just removes the hard
 * edges left by the resample.
 *
 * LS_BLUR_RADIUS is expressed at the build size and divided by the
 * downscale before it reaches the box filter, so the number here stays
 * meaningful if the downscale is ever retuned. The old default of 12 came
 * out as a radius of 2 on a 53px image, which was barely a blur at all —
 * the cover still read as a recognisable picture behind the text rather
 * than as a wash of its colours. This is the default; the live value is
 * ls_settings.blur_radius. */
#define LS_BLUR_DOWNSCALE 6
#define LS_BLUR_RADIUS 36

/* Darkening over the art so lyrics stay readable on bright covers. */
#define LS_SCRIM_ALPHA 0.45

/* -- ranges offered in the settings dialog ---------------------------- */

/* Bounds, not preferences: everything in between should look deliberate,
 * and anything outside should be unreachable rather than merely unwise.
 * They are also applied when loading the config, so a hand-edited value
 * cannot put the panel into a state the dialog can't get it out of. */
#define LS_TEXT_PX_MIN 10
#define LS_TEXT_PX_MAX 72
#define LS_BLUR_RADIUS_MAX 96
#define LS_TRANSITION_MS_MIN 0
#define LS_TRANSITION_MS_MAX 1200

/* -- easing ----------------------------------------------------------- */

/* OutCubic, matching QEasingCurve::OutCubic. */
static inline double ls_ease_out_cubic(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

/* Advance `current` toward `target` along an eased ramp of `duration_ms`.
 * `elapsed_ms` is the time since the animation started. */
static inline double ls_interp(double from, double to, double elapsed_ms,
                               double duration_ms) {
    if (duration_ms <= 0.0) return to;
    return from + (to - from) * ls_ease_out_cubic(elapsed_ms / duration_ms);
}

#endif /* LS_ANIMATIONS_H */
