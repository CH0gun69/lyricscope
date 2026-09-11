#include "settings.h"

#include "animations.h"
#include "log.h"
#include "theme.h"

#include <deadbeef/deadbeef.h>

#include <stdio.h>
#include <string.h>

extern DB_functions_t *deadbeef;

/* Every key is prefixed, and the prefix is this plugin's id.
 *
 * Worth being deliberate about: lyricbar, the other lyrics plugin
 * installed here, writes most of its keys as "lyricbar.<name>" but stores
 * one as a bare "save_method" in the shared namespace. That is exactly the
 * kind of key that collides with some future plugin, so nothing here is
 * ever written unprefixed. */
#define KEY_TEXT_PX "lyricscope.text_px"
#define KEY_ACCENT "lyricscope.accent"
#define KEY_BLUR "lyricscope.blur_radius"
#define KEY_TRANSITION "lyricscope.transition_ms"
#define KEY_ALIGN "lyricscope.alignment"

LsSettings ls_settings = {
    .text_px = (int)LS_LINE_PX,
    .accent = LS_ACCENT_DEFAULT,
    .blur_radius = LS_BLUR_RADIUS,
    .transition_ms = (int)LS_LINE_TRANSITION_MS,
    .alignment = LS_ALIGN_CENTRE,
};

static int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

void ls_settings_load(void) {
    ls_settings.text_px =
        clamp_int(deadbeef->conf_get_int(KEY_TEXT_PX, (int)LS_LINE_PX),
                  LS_TEXT_PX_MIN, LS_TEXT_PX_MAX);
    ls_settings.blur_radius =
        clamp_int(deadbeef->conf_get_int(KEY_BLUR, LS_BLUR_RADIUS), 0,
                  LS_BLUR_RADIUS_MAX);
    ls_settings.transition_ms =
        clamp_int(deadbeef->conf_get_int(KEY_TRANSITION, (int)LS_LINE_TRANSITION_MS),
                  LS_TRANSITION_MS_MIN, LS_TRANSITION_MS_MAX);
    ls_settings.alignment =
        clamp_int(deadbeef->conf_get_int(KEY_ALIGN, LS_ALIGN_CENTRE),
                  LS_ALIGN_LEFT, LS_ALIGN_RIGHT);

    /* conf_get_str, not conf_get_str_fast: the fast one returns a pointer
     * into the config table and is documented as unsafe outside a
     * conf_lock/conf_unlock pair. Copying into our own buffer avoids
     * needing the lock at all. */
    deadbeef->conf_get_str(KEY_ACCENT, LS_ACCENT_DEFAULT, ls_settings.accent,
                           sizeof(ls_settings.accent));

    LS_LOG("settings: text=%dpx accent=%s blur=%d transition=%dms align=%d",
           ls_settings.text_px, ls_settings.accent, ls_settings.blur_radius,
           ls_settings.transition_ms, ls_settings.alignment);
}

void ls_settings_save(void) {
    deadbeef->conf_set_int(KEY_TEXT_PX, ls_settings.text_px);
    deadbeef->conf_set_str(KEY_ACCENT, ls_settings.accent);
    deadbeef->conf_set_int(KEY_BLUR, ls_settings.blur_radius);
    deadbeef->conf_set_int(KEY_TRANSITION, ls_settings.transition_ms);
    deadbeef->conf_set_int(KEY_ALIGN, ls_settings.alignment);

    /* Explicitly, rather than lyricbar's approach of relying on DeaDBeeF
     * flushing the config at exit. That works right up until the player is
     * killed rather than quit, and then the settings the user just chose
     * are gone with no hint as to why. */
    deadbeef->conf_save();
    LS_LOG("settings saved");
}

void ls_settings_accent_rgb(double *r, double *g, double *b) {
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, ls_settings.accent)) {
        gdk_rgba_parse(&rgba, LS_ACCENT_DEFAULT);
    }
    *r = rgba.red;
    *g = rgba.green;
    *b = rgba.blue;
}

PangoAlignment ls_settings_pango_align(void) {
    switch (ls_settings.alignment) {
    case LS_ALIGN_LEFT:
        return PANGO_ALIGN_LEFT;
    case LS_ALIGN_RIGHT:
        return PANGO_ALIGN_RIGHT;
    default:
        return PANGO_ALIGN_CENTER;
    }
}
