/* A spectrum panel coloured from the album art.
 *
 * This does no audio analysis whatsoever, and that is the point rather
 * than a detail. DeaDBeeF's own visualisation engine runs the FFT and
 * hands over finished frequency-domain data through vis_spectrum_listen2;
 * everything here reads those bins and paints them. There is no window
 * function, no transform and no sample-domain code in this file to get
 * wrong, so the bar heights are the player's own numbers.
 *
 * It is additive: DeaDBeeF's Spectrum widget is untouched and still works.
 * This registers a second, separate widget type.
 *
 * The layout deliberately mirrors that stock widget — same octave banding,
 * same gap-as-a-fraction-of-bar-width, same menu. That widget has no
 * source to copy (no deb-src; it is compiled into ddb_gui_GTK3.so), so the
 * modes and their labels were read off its live right-click menu and its
 * bar geometry was measured off a screenshot: ~240 bands across the width,
 * which is 20Hz to Nyquist at 1/24 octave.
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
#include "settings.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern DB_functions_t *deadbeef;
extern ddb_gtkui_t *gtkui_plugin;

/* Upper bound on bands. 1/24 octave over the audible range is about 240,
 * so this has room for finer modes without ever being reached. */
#define MAX_BARS 1024
#define MAX_BINS 4096

/* Where the band layout starts. 20Hz is the bottom of hearing and the
 * value that makes the band count come out at the stock widget's. */
#define BAND_MIN_HZ 20.0

/* Display range. Anything quieter than the floor is simply not drawn. */
#define DB_FLOOR (-70.0)

/* Bars fall at most this much per frame, so a transient decays visibly
 * instead of vanishing between one callback and the next. Purely a
 * display smoothing of values already computed — it never feeds back into
 * anything the player gave us. */
#define FALL_PER_FRAME 0.045

struct LsSpectrum {
    ddb_gtkui_widget_t base;
    GtkWidget *container;
    GtkWidget *area;

    /* Written by the audio thread, read by the GTK thread. */
    GMutex lock;
    float bins[MAX_BINS];
    int nbins;
    int samplerate;

    /* GTK thread only. */
    double bars[MAX_BARS];
    int nbars;
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
        /* Needed to turn a bin index into a frequency, which is what the
         * octave band edges are expressed in. */
        if (data->fmt && data->fmt->samplerate > 0) {
            self->samplerate = data->fmt->samplerate;
        }
        g_mutex_unlock(&self->lock);
    }
    g_mutex_unlock(&live_lock);
}

/* -- bins to bars ------------------------------------------------------ */

/* The level of a band, given its edges in fractional bin units.
 *
 * Two regimes, because a band can be wider or narrower than a bin:
 *
 * WIDER (the treble): take the loudest bin the band covers. Peak rather
 * than summed energy, decided by measurement rather than taste. Energy is
 * the textbook way to build an octave band, and it was tried — but
 * measured against the stock widget at matched playback positions it came
 * out consistently taller, because summing lifts wide high-frequency bands
 * by roughly 10*log10(bins-in-band).
 *
 * NARROWER (the bass): interpolate between the two nearest bins at the
 * band's centre. This is the whole point of taking fractional edges. At
 * 1/24 octave a band is only 2.9% wide, so below ~184Hz (at 5.4Hz per bin)
 * several consecutive bands land inside one bin — measured, the first 34
 * bars resolved to just 7 distinct bins, with 8 bars in a row reading bin
 * 4. Rounding each to a bin index hands back literally the same number,
 * which is what drew the bass as blocky steps. Interpolating makes the
 * value move continuously with the band's centre frequency, so adjacent
 * bars differ again.
 *
 * Either way this only selects or combines numbers the engine produced;
 * nothing is recomputed from audio. */
static double band_amplitude(const float *bins, int nbins, double x_lo,
                             double x_hi) {
    const double top = nbins - 1.0;
    if (x_lo < 1.0) x_lo = 1.0;
    if (x_hi > top) x_hi = top;
    if (x_hi < x_lo) x_hi = x_lo;

    if (x_hi - x_lo >= 1.0) {
        int lo = (int)floor(x_lo);
        int hi = (int)ceil(x_hi);
        if (lo < 1) lo = 1;
        if (hi > nbins) hi = nbins;
        if (hi <= lo) hi = lo + 1;
        if (hi > nbins) return 0.0;

        double peak = 0.0;
        for (int k = lo; k < hi; k++) {
            const double v = fabs((double)bins[k]);
            if (v > peak) {
                peak = v;
            }
        }
        return peak;
    }

    double x = (x_lo + x_hi) * 0.5;
    if (x < 1.0) x = 1.0;
    if (x > top) x = top;
    int i = (int)floor(x);
    if (i < 1) i = 1;
    if (i > nbins - 2) i = nbins - 2;
    if (i < 1) return 0.0;

    const double frac = x - i;
    const double a = fabs((double)bins[i]);
    const double b = fabs((double)bins[i + 1]);
    return a + (b - a) * frac;
}

static double to_level(double peak) {
    const double db = peak > 1e-9 ? 20.0 * log10(peak) : DB_FLOOR;
    double level = (db - DB_FLOOR) / (0.0 - DB_FLOOR);
    if (level < 0.0) level = 0.0;
    if (level > 1.0) level = 1.0;
    return level;
}

/* Group the engine's bins into bars.
 *
 * This is presentation, not analysis: it selects among numbers the engine
 * already produced and never recomputes one. Octave banding rather than
 * anything linear because that is what the stock widget does, and because
 * linearly spaced bins put almost all of a track's audible detail in the
 * leftmost few pixels. Loudest-in-span rather than mean because a mean
 * over a wide high-frequency span washes peaks out until the right-hand
 * half of the display barely moves. */
static int compute_bars(LsSpectrum *self, double out[MAX_BARS]) {
    float bins[MAX_BINS];
    int nbins, samplerate;

    g_mutex_lock(&self->lock);
    nbins = self->nbins;
    samplerate = self->samplerate;
    if (nbins > 0) {
        memcpy(bins, self->bins, (size_t)nbins * sizeof(float));
    }
    g_mutex_unlock(&self->lock);

    if (nbins <= 1) {
        return 0;
    }

    if (ls_settings.spectrum_mode == LS_SPEC_DISCRETE) {
        /* One bar per bin, exactly as delivered — no grouping at all. */
        int nbars = nbins - 1;
        if (nbars > MAX_BARS) {
            nbars = MAX_BARS;
        }
        for (int i = 0; i < nbars; i++) {
            out[i] = to_level(fabs((double)bins[i + 1]));
        }
        return nbars;
    }

    const double denom = (ls_settings.spectrum_mode == LS_SPEC_OCT12) ? 12.0 : 24.0;
    const double nyquist = (samplerate > 0 ? samplerate : 44100) / 2.0;
    if (nyquist <= BAND_MIN_HZ) {
        return 0;
    }
    /* Hz per bin: the engine's bins span DC to Nyquist. */
    const double hz_per_bin = nyquist / nbins;

    /* LS_DEBUG_BINS: report which source bin each of the low bars actually
     * resolves to. Runs of the same index are what a blocky bass looks
     * like from the inside. Rate-limited; costs nothing when unset. */
    static int debug_bins = -1;
    if (debug_bins < 0) {
        debug_bins = getenv("LS_DEBUG_BINS") ? 1 : 0;
    }
    char trace[512];
    int traced = 0;
    trace[0] = 0;

    int nbars = 0;
    for (int k = 0; nbars < MAX_BARS; k++) {
        const double f_lo = BAND_MIN_HZ * pow(2.0, k / denom);
        const double f_hi = BAND_MIN_HZ * pow(2.0, (k + 1) / denom);
        if (f_lo >= nyquist) {
            break;
        }
        /* Fractional, not rounded: the rounding is exactly what flattened
         * the bass into steps. */
        const double x_lo = f_lo / hz_per_bin;
        const double x_hi = f_hi / hz_per_bin;
        if (debug_bins && nbars < 34) {
            traced += snprintf(trace + traced, sizeof(trace) - traced, "%.2f ",
                               (x_lo + x_hi) * 0.5);
        }
        out[nbars++] = to_level(band_amplitude(bins, nbins, x_lo, x_hi));
    }

    if (debug_bins) {
        static gint64 last = 0;
        const gint64 now = g_get_monotonic_time();
        if (now - last > 2000000) {
            last = now;
            char levels[1400];
            int n = 0;
            levels[0] = 0;
            for (int i = 0; i < 48 && i < nbars; i++) {
                n += snprintf(levels + n, sizeof(levels) - n, "%.3f ", out[i]);
            }
            LS_LOG("nbins=%d hz_per_bin=%.2f | band centre in bins, bars 0-33: %s",
                   nbins, hz_per_bin, trace);
            LS_LOG("  bass bar levels 0-47: %s", levels);
        }
    }
    return nbars;
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

static void gradient_stops(const LsSpectrum *self, cairo_pattern_t *pattern) {
    if (self->palette.valid) {
        /* Dark at the bottom, light at the top: a bar then reads as rising
         * *into* the bright tone, and short bars stay dark and recede,
         * which is what makes the tall ones legible at a glance. */
        cairo_pattern_add_color_stop_rgba(pattern, 0.0, self->palette.lo[0],
                                          self->palette.lo[1],
                                          self->palette.lo[2], 1.0);
        cairo_pattern_add_color_stop_rgba(pattern, LS_GRAD_MID_STOP,
                                          self->palette.mid[0],
                                          self->palette.mid[1],
                                          self->palette.mid[2], 1.0);
        /* The last stop, so cairo holds the highlight from here to the top
         * of the bar rather than continuing to ramp. */
        cairo_pattern_add_color_stop_rgba(pattern, LS_GRAD_HI_STOP,
                                          self->palette.hi[0],
                                          self->palette.hi[1],
                                          self->palette.hi[2], 1.0);
        return;
    }
    /* No art yet: the panel's own accent, dimmed at the bottom. */
    cairo_pattern_add_color_stop_rgba(pattern, 0.0, 0.16, 0.20, 0.30, 1.0);
    cairo_pattern_add_color_stop_rgba(pattern, LS_GRAD_HI_STOP, 0.54, 0.71,
                                      1.00, 1.0);
}

/* -- drawing ----------------------------------------------------------- */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    LsSpectrum *self = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    cairo_set_source_rgba(cr, LS_COLOUR_BACKDROP);
    cairo_paint(cr);

    if (alloc.width <= 0 || alloc.height <= 0 || self->nbars <= 0) {
        return FALSE;
    }

    /* One gradient for the whole panel, not one per bar: the stops are in
     * panel space, so every bar is a window onto the same vertical ramp
     * and a tall bar and a short one agree about what colour a given
     * height is. Per-bar gradients would make every bar span the full
     * ramp and the picture would say nothing about level. */
    cairo_pattern_t *pattern =
        cairo_pattern_create_linear(0, alloc.height, 0, 0);
    gradient_stops(self, pattern);
    cairo_set_source(cr, pattern);

    /* Gap is a fraction of the *bar*, not of the slot, which is what the
     * stock widget's "1/N Bar" means. So slot = bar + bar/N, and the bar
     * gets N/(N+1) of its slot. */
    const double slot = (double)alloc.width / self->nbars;
    const int n = ls_settings.spectrum_gap;
    const double bar_w = (n <= 0) ? slot : slot * ((double)n / (n + 1.0));

    for (int i = 0; i < self->nbars; i++) {
        const double h = self->bars[i] * alloc.height;
        if (h < 1.0) {
            continue;
        }
        double w = bar_w;
        /* Below a pixel the gap would erase the bar entirely; a hairline
         * bar with no gap is the honest rendering of "too many bands for
         * this width". */
        if (w < 1.0) {
            w = 1.0;
        }
        cairo_rectangle(cr, i * slot, alloc.height - h, w, h);
    }
    cairo_fill(cr);
    cairo_pattern_destroy(pattern);
    return FALSE;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data) {
    (void)clock;
    LsSpectrum *self = user_data;

    double target[MAX_BARS];
    const int nbars = compute_bars(self, target);

    if (nbars != self->nbars) {
        /* Mode change, or the first data to arrive. Nothing sensible to
         * decay from, so land on the new values rather than sliding bars
         * that now mean a different frequency. */
        for (int i = 0; i < nbars; i++) {
            self->bars[i] = target[i];
        }
        self->nbars = nbars;
    } else {
        for (int i = 0; i < nbars; i++) {
            if (target[i] >= self->bars[i]) {
                self->bars[i] = target[i]; /* rises instantly */
            } else {
                self->bars[i] -= FALL_PER_FRAME;
                if (self->bars[i] < target[i]) {
                    self->bars[i] = target[i];
                }
            }
        }
    }

    refresh_palette(self);
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

/* -- right-click menu -------------------------------------------------- */

/* Mirrors the stock Spectrum widget's menu: the same two submenus, the
 * same labels, the same radio-group shape. Someone switching between the
 * two panels should not have to learn a second vocabulary. */

typedef struct {
    LsSpectrum *self;
    int value;
} LsMenuChoice;

static void on_mode_chosen(GtkCheckMenuItem *item, gpointer data) {
    LsMenuChoice *choice = data;
    if (!gtk_check_menu_item_get_active(item)) {
        return; /* the half of the radio pair being switched off */
    }
    ls_settings.spectrum_mode = choice->value;
    ls_settings_save();
    /* The band count is about to change; let the tick rebuild it. */
    choice->self->nbars = 0;
    gtk_widget_queue_draw(choice->self->area);
}

static void on_gap_chosen(GtkCheckMenuItem *item, gpointer data) {
    LsMenuChoice *choice = data;
    if (!gtk_check_menu_item_get_active(item)) {
        return;
    }
    ls_settings.spectrum_gap = choice->value;
    ls_settings_save();
    gtk_widget_queue_draw(choice->self->area);
}

/* A real function rather than casting g_free to GClosureNotify: the two
 * signatures genuinely differ, and the compiler is right to say so. */
static void free_choice(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static GtkWidget *radio_item(GSList **group, const char *label, int active,
                             LsSpectrum *self, int value, GCallback handler) {
    GtkWidget *item = gtk_radio_menu_item_new_with_label(*group, label);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);

    LsMenuChoice *choice = g_malloc0(sizeof(LsMenuChoice));
    choice->self = self;
    choice->value = value;
    /* Freed with the menu item, so the callback data cannot outlive the
     * widget that can invoke it. */
    g_signal_connect_data(item, "toggled", handler, choice, free_choice, 0);
    return item;
}

static void show_context_menu(LsSpectrum *self, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new();

    /* Rendering Mode */
    GtkWidget *mode_item = gtk_menu_item_new_with_label("Rendering Mode");
    GtkWidget *mode_menu = gtk_menu_new();
    GSList *mode_group = NULL;
    static const struct {
        const char *label;
        int value;
    } modes[] = {
        {"Discrete Frequencies", LS_SPEC_DISCRETE},
        {"1/24 Octave Bands", LS_SPEC_OCT24},
        {"1/12 Octave Bands", LS_SPEC_OCT12},
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(modes); i++) {
        gtk_menu_shell_append(
            GTK_MENU_SHELL(mode_menu),
            radio_item(&mode_group, modes[i].label,
                       ls_settings.spectrum_mode == modes[i].value, self,
                       modes[i].value, G_CALLBACK(on_mode_chosen)));
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mode_item), mode_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mode_item);

    /* Gap Size */
    GtkWidget *gap_item = gtk_menu_item_new_with_label("Gap Size");
    GtkWidget *gap_menu = gtk_menu_new();
    GSList *gap_group = NULL;
    gtk_menu_shell_append(GTK_MENU_SHELL(gap_menu),
                          radio_item(&gap_group, "None",
                                     ls_settings.spectrum_gap == 0, self, 0,
                                     G_CALLBACK(on_gap_chosen)));
    for (int n = 2; n <= 10; n++) {
        char label[16];
        snprintf(label, sizeof(label), "1/%d Bar", n);
        gtk_menu_shell_append(
            GTK_MENU_SHELL(gap_menu),
            radio_item(&gap_group, label, ls_settings.spectrum_gap == n, self,
                       n, G_CALLBACK(on_gap_chosen)));
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(gap_item), gap_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gap_item);

    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy),
                     NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                gpointer user_data) {
    (void)widget;
    LsSpectrum *self = user_data;
    if (event->button != 3) {
        return FALSE;
    }
    /* Design Mode owns right-click while it is on — that menu is how a
     * widget gets moved, replaced or deleted, and quietly replacing it
     * would strand the panel in the layout. */
    if (gtkui_plugin && gtkui_plugin->w_get_design_mode()) {
        return FALSE;
    }
    show_context_menu(self, event);
    return TRUE;
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
    self->samplerate = 44100;

    /* Container for gtkui, drawing area for us — handing gtkui the
     * drawing area directly is what silently breaks input on a widget
     * (see panel.c). */
    self->container = gtk_event_box_new();
    self->area = gtk_drawing_area_new();
    gtk_widget_add_events(self->area, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(self->container), self->area);

    g_signal_connect(self->area, "draw", G_CALLBACK(on_draw), self);
    g_signal_connect(self->area, "button-press-event",
                     G_CALLBACK(on_button_press), self);
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
