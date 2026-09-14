/* Runtime-configurable values, persisted in DeaDBeeF's own config file.
 *
 * The defaults live in animations.h and theme.h, not here: those two
 * headers stay the single place to read "what does this panel look like",
 * and this struct is the runtime overlay on top of them. A value that is
 * unset in the config falls back to its compiled-in default, so a fresh
 * install and an upgraded one look identical.
 */

#ifndef LS_SETTINGS_H
#define LS_SETTINGS_H

#include <gtk/gtk.h>

enum {
    LS_ALIGN_LEFT = 0,
    LS_ALIGN_CENTRE = 1,
    LS_ALIGN_RIGHT = 2,
};

/* Spectrum rendering modes, named and ordered to match the menu on
 * DeaDBeeF's own Spectrum widget exactly — read off the live UI, since
 * that widget has no source to read. */
enum {
    LS_SPEC_DISCRETE = 0, /* "Discrete Frequencies" */
    LS_SPEC_OCT24 = 1,    /* "1/24 Octave Bands" — the stock default */
    LS_SPEC_OCT12 = 2,    /* "1/12 Octave Bands" */
};

typedef struct {
    int text_px;        /* line size at full scale, in device pixels */
    char font[128];     /* Pango description minus the size: "Family Style".
                         * Empty means the theme's own UI font. */
    char accent[16];    /* current line's colour, "#rrggbb" */
    int blur_radius;    /* backdrop blur, at build size; 0 disables it */
    int transition_ms;  /* line expand/shrink duration; easing stays OutCubic */
    int scroll_resume_ms; /* wait after hand-scrolling before re-syncing; 0 = never */
    int alignment;      /* LS_ALIGN_* */

    /* Spectrum panel. Kept here with everything else rather than in a
     * second settings struct: one config namespace, one load, one save. */
    int spectrum_mode;  /* LS_SPEC_* */
    int spectrum_gap;   /* gap as a fraction of bar width: 0 = none,
                         * otherwise N meaning bar_width/N, matching the
                         * stock widget's "1/N Bar" menu. */
} LsSettings;

/* Read by the draw path every frame, so it is a plain global rather than
 * something threaded through every call. Only ever written from the GTK
 * main thread. */
extern LsSettings ls_settings;

void ls_settings_load(void);
void ls_settings_save(void);

/* Accent as cairo components. Falls back to the compiled-in default if the
 * stored string is not a colour GDK can parse. */
void ls_settings_accent_rgb(double *r, double *g, double *b);

/* PANGO_ALIGN_* for the current alignment setting. */
PangoAlignment ls_settings_pango_align(void);

#endif /* LS_SETTINGS_H */
