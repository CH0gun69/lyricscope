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

typedef struct {
    int text_px;        /* line size at full scale, in device pixels */
    char accent[16];    /* current line's colour, "#rrggbb" */
    int blur_radius;    /* backdrop blur, at build size; 0 disables it */
    int transition_ms;  /* line expand/shrink duration; easing stays OutCubic */
    int alignment;      /* LS_ALIGN_* */
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
