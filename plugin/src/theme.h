/* Colours in one place — the C counterpart of ui/styles.qss, carrying the
 * same palette so the docked panel matches the window it replaces.
 *
 * Sizes deliberately live in animations.h instead, for the same reason
 * they were never in the .qss: the line size is a number this code
 * interpolates, not something the toolkit is allowed to own.
 */

#ifndef LS_THEME_H
#define LS_THEME_H

/* r, g, b, a — expanded straight into cairo_set_source_rgba(). */

/* The current line's colour is the one thing the user can change, so it
 * lives in the config rather than here; this is only its default. Read it
 * through ls_settings_accent_rgb(). */
#define LS_ACCENT_DEFAULT "#8ab4ff"

/* Past and future lines stay neutral and deliberately do NOT derive from
 * the accent. Tinting the whole column with it would undo the point of an
 * accent — the current line reads as current because it is the only
 * coloured thing on screen, and a saturated accent applied at 26% and 42%
 * to eighty other lines turns the panel into a wash. Dimming is about
 * legibility; the accent is about attention. */
#define LS_COLOUR_FUTURE 0.914, 0.929, 0.969, 0.42
#define LS_COLOUR_PAST 0.914, 0.929, 0.969, 0.26
#define LS_COLOUR_PLAIN 0.914, 0.929, 0.969, 0.82
#define LS_COLOUR_PLACEHOLDER 0.914, 0.929, 0.969, 0.50

/* Where the spectrum's vertical gradient changes tone, as a fraction of
 * bar height from the bottom.
 *
 * Deliberately not even thirds. The dark tone is an anchor at the base —
 * it gives the bars a foot and keeps short ones from glowing — but it only
 * needs a sliver to read as one, and the mid is a transition rather than a
 * destination. Everything above LS_GRAD_HI_STOP is the highlight, which is
 * the tone that actually carries the album's colour, so it gets the
 * majority of the bar instead of the top third. */
#define LS_GRAD_MID_STOP 0.15
#define LS_GRAD_HI_STOP 0.45

/* Shown behind the art, and on its own when a track has no cover. */
#define LS_COLOUR_BACKDROP 0.047, 0.055, 0.075, 1.00

#endif /* LS_THEME_H */
