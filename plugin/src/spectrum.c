/* A spectrum panel coloured from the album art.
 *
 * This does no audio analysis whatsoever, and that is the point rather
 * than a detail. DeaDBeeF's own visualisation engine runs the FFT and
 * hands over finished frequency-domain data through vis_spectrum_listen2;
 * everything here reads those bins and paints them. There is no window
 * function, no transform and no sample-domain code in this file to get
 * wrong, so the bar heights are the player's own numbers and match the
 * built-in Spectrum tab by construction.
 *
 * It is additive: DeaDBeeF's Spectrum widget is untouched and still works.
 * This registers a second, separate widget type.
 *
 * Two things the API documentation is explicit about and which shape the
 * whole file:
 *
 *   - the callback runs on a *background* thread, so nothing it touches
 *     may be read by the draw handler without a lock, and it must never
 *     call into GTK;
 *   - the number of frequency bins is whatever the engine feels like
 *     producing. DDB_FREQ_BANDS (256) is deprecated and documented as
 *     doing nothing, so the bin count is read from every callback rather
 *     than assumed once.
 */

#include "spectrum.h"

#include "artwork.h"
#include "log.h"
#include "palette.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern DB_functions_t *deadbeef;
extern ddb_gtkui_t *gtkui_plugin;

/* How many bars are drawn, regardless of how many bins arrive. */
#define BARS 72

/* Display range. Anything quieter than the floor is simply not drawn. */
#define DB_FLOOR (-70.0)

/* Bars fall at most this much per frame, so a transient decays visibly
 * instead of vanishing between one callback and the next. Purely a
 * display smoothing of values already computed — it never feeds back into
 * anything the player gave us. */
#define FALL_PER_FRAME 0.045

#define MAX_BINS 4096

struct LsSpectrum {
    ddb_gtkui_widget_t base;
    GtkWidget *container;
    GtkWidget *area;

    /* Written by the audio thread, read by the GTK thread. */
    GMutex lock;
    float bins[MAX_BINS];
    int nbins;

    /* GTK thread only. */
    double bars[BARS];
    LsPalette palette;
    char *art_path;
    guint tick_id;
};

/* Every live panel, so a callback that arrives while one is being
 * destroyed can tell. vis_spectrum_unlisten is called first on destroy,
 * but it says nothing about a callback already in flight on the audio
 * thread, and this is the same guard the artwork path already uses for
 * the same reason. */
static GSList *live_spectrums;
static GMutex live_lock;

/* -- the audio thread -------------------------------------------------- */

static void spectrum_callback(void *ctx, const ddb_audio_data_t *data) {
    LsSpectrum *self = ctx;

    g_mutex_lock(&live_lock);
    if (!g_slist_find(live_spectrums, self)) {
        g_mutex_unlock(&live_lock);
        return; /* destroyed while this was in flight */
    }

    if (data && data->data && data->nframes > 0) {
        g_mutex_lock(&self->lock);
        /* Channel 0 only. The data is planar, so the first nframes floats
         * are one whole channel — mixing channels would change the
         * numbers the player produced, and this panel's job is to show
         * them, not to have opinions about them. */
        const int n = data->nframes > MAX_BINS ? MAX_BINS : data->nframes;
        memcpy(self->bins, data->data, (size_t)n * sizeof(float));
        self->nbins = n;
        g_mutex_unlock(&self->lock);
    }
    g_mutex_unlock(&live_lock);
}

/* -- bins to bars ------------------------------------------------------ */

/* Map the bin range onto BARS logarithmically and take the loudest bin in
 * each bar's span.
 *
 * This is presentation, not analysis: it selects among numbers the engine
 * already produced and never recomputes one. Logarithmic because linearly
 * spaced bins put almost all of a track's audible detail in the leftmost
 * few pixels; loudest-in-span rather than mean because a mean over a wide
 * high-frequency span washes peaks out until the right-hand half of the
 * display barely moves. */
static void bins_to_bars(LsSpectrum *self, double out[BARS]) {
    float bins[MAX_BINS];
    int nbins;

    g_mutex_lock(&self->lock);
    nbins = self->nbins;
    if (nbins > 0) {
        memcpy(bins, self->bins, (size_t)nbins * sizeof(float));
    }
    g_mutex_unlock(&self->lock);

    if (nbins <= 1) {
        for (int i = 0; i < BARS; i++) {
            out[i] = 0.0;
        }
        return;
    }

    const double top = nbins - 1;
    for (int i = 0; i < BARS; i++) {
        /* Bin 1 upward: bin 0 is DC and is not music. */
        const double a = pow(top, (double)i / BARS);
        const double b = pow(top, (double)(i + 1) / BARS);
        int lo = (int)floor(a);
        int hi = (int)ceil(b);
        if (lo < 1) lo = 1;
        if (hi > nbins) hi = nbins;
        if (hi <= lo) hi = lo + 1;
        if (hi > nbins) hi = nbins;

        double peak = 0.0;
        for (int k = lo; k < hi; k++) {
            const double v = fabs((double)bins[k]);
            if (v > peak) {
                peak = v;
            }
        }

        const double db = peak > 1e-9 ? 20.0 * log10(peak) : DB_FLOOR;
        double level = (db - DB_FLOOR) / (0.0 - DB_FLOOR);
        if (level < 0.0) level = 0.0;
        if (level > 1.0) level = 1.0;
        out[i] = level;
    }
}

/* -- colours ----------------------------------------------------------- */

/* Refresh the cached palette if the playing track's art has changed.
 *
 * Cheap enough to call per frame: it is a string compare against the
 * cache, and the extraction itself only ever runs in the backdrop build. */
static void refresh_palette(LsSpectrum *self) {
    DB_playItem_t *track = deadbeef->streamer_get_playing_track_safe();
    if (!track) {
        return;
    }
    deadbeef->pl_lock();
    const char *uri = deadbeef->pl_find_meta(track, ":URI");
    char *path = uri ? strdup(uri) : NULL;
    deadbeef->pl_unlock();
    deadbeef->pl_item_unref(track);

    if (!path) {
        return;
    }
    if (self->art_path && strcmp(self->art_path, path) == 0) {
        free(path);
        return; /* same track; the palette we have is the right one */
    }

    LsPalette palette;
    if (ls_palette_cache_get(path, &palette)) {
        self->palette = palette;
        free(self->art_path);
        self->art_path = path;
        return;
    }
    /* The cover has not been decoded yet — the backdrop build fills the
     * cache and may not have run. Keep the previous colours and try again
     * next frame rather than flashing back to the fallback. */
    free(path);
}

static void gradient_stops(const LsSpectrum *self, cairo_pattern_t *pattern,
                           double height) {
    if (self->palette.valid) {
        /* Dark at the bottom, light at the top: a bar then reads as rising
         * *into* the bright tone, and short bars stay dark and recede,
         * which is what makes the tall ones legible at a glance. */
        cairo_pattern_add_color_stop_rgba(pattern, 0.0, self->palette.lo[0],
                                          self->palette.lo[1],
                                          self->palette.lo[2], 1.0);
        cairo_pattern_add_color_stop_rgba(pattern, 0.5, self->palette.mid[0],
                                          self->palette.mid[1],
                                          self->palette.mid[2], 1.0);
        cairo_pattern_add_color_stop_rgba(pattern, 1.0, self->palette.hi[0],
                                          self->palette.hi[1],
                                          self->palette.hi[2], 1.0);
        return;
    }
    (void)height;
    /* No art yet: the panel's own accent, dimmed at the bottom. */
    cairo_pattern_add_color_stop_rgba(pattern, 0.0, 0.16, 0.20, 0.30, 1.0);
    cairo_pattern_add_color_stop_rgba(pattern, 1.0, 0.54, 0.71, 1.00, 1.0);
}

/* -- drawing ----------------------------------------------------------- */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    LsSpectrum *self = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    cairo_set_source_rgba(cr, LS_COLOUR_BACKDROP);
    cairo_paint(cr);

    if (alloc.width <= 0 || alloc.height <= 0) {
        return FALSE;
    }

    /* One gradient for the whole panel, not one per bar: the stops are in
     * panel space, so every bar is a window onto the same vertical ramp
     * and a tall bar and a short one agree about what colour a given
     * height is. Per-bar gradients would make every bar span the full
     * ramp and the picture would say nothing about level. */
    cairo_pattern_t *pattern =
        cairo_pattern_create_linear(0, alloc.height, 0, 0);
    gradient_stops(self, pattern, alloc.height);
    cairo_set_source(cr, pattern);

    const double slot = (double)alloc.width / BARS;
    const double gap = slot > 4.0 ? 1.0 : 0.0;
    for (int i = 0; i < BARS; i++) {
        const double h = self->bars[i] * alloc.height;
        if (h < 1.0) {
            continue;
        }
        cairo_rectangle(cr, i * slot, alloc.height - h, slot - gap, h);
    }
    cairo_fill(cr);
    cairo_pattern_destroy(pattern);
    return FALSE;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data) {
    (void)clock;
    LsSpectrum *self = user_data;

    double target[BARS];
    bins_to_bars(self, target);

    for (int i = 0; i < BARS; i++) {
        if (target[i] >= self->bars[i]) {
            self->bars[i] = target[i]; /* rises instantly */
        } else {
            self->bars[i] -= FALL_PER_FRAME;
            if (self->bars[i] < target[i]) {
                self->bars[i] = target[i];
            }
        }
    }

    refresh_palette(self);
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* -- lifecycle --------------------------------------------------------- */

static void spectrum_destroy(ddb_gtkui_widget_t *widget) {
    LsSpectrum *self = (LsSpectrum *)widget;

    /* Unregister before anything is freed, then drop out of the live list
     * under the same lock the callback takes — after this returns, a
     * callback either already finished or will find nothing and leave. */
    deadbeef->vis_spectrum_unlisten(self);

    g_mutex_lock(&live_lock);
    live_spectrums = g_slist_remove(live_spectrums, self);
    g_mutex_unlock(&live_lock);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(self->area, self->tick_id);
        self->tick_id = 0;
    }
    free(self->art_path);
    self->art_path = NULL;
    g_mutex_clear(&self->lock);
}

LsSpectrum *ls_spectrum_new(void) {
    LsSpectrum *self = calloc(1, sizeof(LsSpectrum));
    if (!self) {
        return NULL;
    }
    g_mutex_init(&self->lock);

    /* Container for gtkui, drawing area for us — handing gtkui the
     * drawing area directly is what silently breaks input on a widget
     * (see panel.c). */
    self->container = gtk_event_box_new();
    self->area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(self->container), self->area);

    g_signal_connect(self->area, "draw", G_CALLBACK(on_draw), self);
    self->tick_id =
        gtk_widget_add_tick_callback(self->area, on_tick, self, NULL);

    self->base.widget = self->container;
    self->base.destroy = spectrum_destroy;

    g_mutex_lock(&live_lock);
    live_spectrums = g_slist_prepend(live_spectrums, self);
    g_mutex_unlock(&live_lock);

    /* Listed only once the panel is fully built and findable, so the first
     * callback cannot arrive before there is somewhere to put it. */
    deadbeef->vis_spectrum_listen2(self, spectrum_callback);

    gtk_widget_show_all(self->container);
    return self;
}

ddb_gtkui_widget_t *ls_spectrum_as_widget(LsSpectrum *self) {
    return &self->base;
}
