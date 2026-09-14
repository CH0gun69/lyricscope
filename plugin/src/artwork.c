/* Blurred cover art behind the lyrics.
 *
 * Cover lookup goes through DeaDBeeF's own artwork plugin rather than
 * hunting for files: in-process that API already covers embedded pictures,
 * folder images and the player's disk cache, and it is the same art the
 * rest of the player shows. The standalone version had to reimplement all
 * of that from the outside.
 *
 * The blur itself is three box-blur passes on a downscaled copy. Three
 * boxes approximate a gaussian closely enough that nothing here is
 * distinguishable from one, and doing it at 1/6 scale makes the cost
 * negligible — most of the smoothing is the downscale, and the blur only
 * removes the hard edges the resample leaves behind.
 */

#include "artwork.h"

#include "animations.h"
#include "log.h"
#include "settings.h"
#include "theme.h"

#include <deadbeef/artwork.h>
#include <stdlib.h>
#include <string.h>

extern DB_functions_t *deadbeef;

/* The backdrop is built at this size regardless of the panel's, then
 * scaled up at draw time. It is blurred, so there is nothing for the
 * upscale to lose. */
#define ART_BUILD_PX 320

static ddb_artwork_plugin_t *artwork_plugin;

/* Which LsArtwork instances are still alive.
 *
 * A cover query is answered on the artwork plugin's own thread and handed
 * to the GTK loop as an idle, so the answer can outlive the panel that
 * asked for it — and install_backdrop would then write four fields into
 * freed memory, quietly corrupting whatever the allocator handed out
 * next. cancel_queries_with_source_id closes most of that window; this
 * list closes the rest, because an idle that is already queued is past
 * cancelling. Touched only from the GTK main thread: the queries are
 * started from it and the idles run on it. */
static GSList *live_artworks;

void ls_artwork_init(void) {
    /* "artwork2", not "artwork" — the file is artwork.so but the plugin
     * inside it registers under the v2 id, so the obvious guess silently
     * returns NULL and leaves the backdrop flat with no other symptom. */
    artwork_plugin =
        (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id("artwork2");
    if (!artwork_plugin) {
        artwork_plugin =
            (ddb_artwork_plugin_t *)deadbeef->plug_get_for_id("artwork");
    }
    if (!artwork_plugin) {
        LS_LOG("artwork plugin not available; backdrop will stay flat");
        return;
    }
    LS_LOG("artwork plugin found (api %d.%d)",
           artwork_plugin->plugin.plugin.version_major,
           artwork_plugin->plugin.plugin.version_minor);
}

/* -- blur ------------------------------------------------------------- */

/* One box-blur pass, separable, done in place through a scratch buffer. */
static void box_blur_pass(guchar *src, guchar *dst, int width, int height,
                          int stride, int channels, int radius,
                          int horizontal) {
    const int outer = horizontal ? height : width;
    const int inner = horizontal ? width : height;
    const int step = horizontal ? channels : stride;
    const int line = horizontal ? stride : channels;

    for (int o = 0; o < outer; o++) {
        guchar *row = src + (gsize)o * line;
        guchar *out = dst + (gsize)o * line;
        for (int c = 0; c < channels; c++) {
            int sum = 0;
            int count = 0;
            /* Prime the window with the first `radius` samples. */
            for (int i = 0; i <= radius && i < inner; i++) {
                sum += row[(gsize)i * step + c];
                count++;
            }
            for (int i = 0; i < inner; i++) {
                out[(gsize)i * step + c] = (guchar)(sum / count);
                const int add = i + radius + 1;
                const int drop = i - radius;
                if (add < inner) {
                    sum += row[(gsize)add * step + c];
                    count++;
                }
                if (drop >= 0) {
                    sum -= row[(gsize)drop * step + c];
                    count--;
                }
            }
        }
    }
}

static void blur_pixbuf(GdkPixbuf *pixbuf, int radius) {
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

    guchar *scratch = malloc((gsize)stride * height);
    if (!scratch) {
        return;
    }
    for (int pass = 0; pass < 3; pass++) {
        box_blur_pass(pixels, scratch, width, height, stride, channels, radius, 1);
        box_blur_pass(scratch, pixels, width, height, stride, channels, radius, 0);
    }
    free(scratch);
}

/* -- LS_DEBUG_BLUR ----------------------------------------------------- *
 *
 * The backdrop was once seen painting a flat wash of a cover's average
 * colour instead of the blur of it, in a player that had been running for
 * a while — and it came back correct on restart, with the same track, the
 * same cover and the same radius. That is a state bug, and state bugs are
 * not findable after the fact: by the time it is noticed, the interesting
 * moment is hours gone.
 *
 * So this reports the two things that tell the halves apart. Every build
 * logs what went in and what came out, and dumps the surface to a file,
 * which settles whether the *built* image is blurred. Every draw logs the
 * fade and the spread of the surface being painted, which settles whether
 * a good surface is being painted badly. A flat dump means the build is
 * wrong; a varied surface drawn onto a flat panel means the draw is.
 *
 * Off unless LS_DEBUG_BLUR is set in the environment, and the getenv is
 * cached — the draw path runs per frame and this must cost nothing when
 * it is off. */
static int blur_debug(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LS_DEBUG_BLUR") ? 1 : 0;
    }
    return enabled;
}

/* Per-channel min-to-max across a surface. Flat means one colour, which
 * is the symptom being hunted; anything above a few units per channel is
 * a blur that survived. ARGB32 is premultiplied BGRA in memory, and the
 * backdrop is opaque, so the three channels read out as B, G, R. */
static void surface_spread(cairo_surface_t *surface, int spread[3]) {
    spread[0] = spread[1] = spread[2] = -1;
    if (!surface ||
        cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32) {
        return;
    }
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    if (!data) {
        return;
    }
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int y = 0; y < height; y++) {
        const unsigned char *row = data + (gsize)y * stride;
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < 3; c++) {
                const unsigned char v = row[x * 4 + c];
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
            }
        }
    }
    for (int c = 0; c < 3; c++) {
        spread[c] = hi[c] - lo[c];
    }
}

static cairo_surface_t *build_backdrop(const char *filename) {
    GError *error = NULL;
    GdkPixbuf *full = gdk_pixbuf_new_from_file(filename, &error);
    if (!full) {
        LS_LOG("cover load failed: %s", error ? error->message : "?");
        g_clear_error(&error);
        return NULL;
    }

    /* Downscale first: this is most of the smoothing and nearly all of the
     * speed, since everything after it works on 1/36th the pixels. */
    const int small = MAX(ART_BUILD_PX / LS_BLUR_DOWNSCALE, 8);
    GdkPixbuf *tiny =
        gdk_pixbuf_scale_simple(full, small, small, GDK_INTERP_BILINEAR);
    g_object_unref(full);
    if (!tiny) {
        return NULL;
    }

    /* The configured radius is expressed at the build size, so it is
     * divided down to the size the filter actually runs at. A radius that
     * divides to nothing is treated as "no blur" rather than silently
     * rounded up to 1 — the setting offers 0 as a real choice, and the
     * user asking for a sharp cover should get one. */
    const int radius = ls_settings.blur_radius / LS_BLUR_DOWNSCALE;
    if (radius >= 1) {
        blur_pixbuf(tiny, radius);
    }

    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(tiny, ART_BUILD_PX,
                                                ART_BUILD_PX, GDK_INTERP_BILINEAR);
    g_object_unref(tiny);
    if (!scaled) {
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, ART_BUILD_PX, ART_BUILD_PX);
    cairo_t *cr = cairo_create(surface);
    gdk_cairo_set_source_pixbuf(cr, scaled, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(scaled);
    cairo_surface_flush(surface);

    if (blur_debug()) {
        static int build = 0;
        int spread[3];
        surface_spread(surface, spread);
        /* The counter is in the filename rather than one file overwritten
         * each time, because "it went flat after a while" is a claim about
         * a sequence of builds, and only keeping them all can show which
         * one turned. */
        char *dump = g_strdup_printf("%s/lyricscope-backdrop-%03d.png",
                                     g_get_tmp_dir(), ++build);
        cairo_surface_write_to_png(surface, dump);
        LS_LOG("blur build #%d: radius=%d/%d at %dpx, source %s -> spread "
               "B=%d G=%d R=%d, dumped to %s",
               build, ls_settings.blur_radius / LS_BLUR_DOWNSCALE,
               ls_settings.blur_radius, small, filename, spread[0], spread[1],
               spread[2], dump);
        g_free(dump);
    }
    return surface;
}

/* -- async plumbing --------------------------------------------------- */

typedef struct {
    LsArtwork *art;
    char *filename;
} LsCoverReady;

static gboolean install_backdrop(gpointer data) {
    LsCoverReady *ready = data;
    LsArtwork *art = ready->art;

    /* The panel that asked for this is gone. Its memory may well have
     * been handed out again by now, so `art` is not something to write
     * to, or even to read a flag out of — the list is the only thing
     * that can still be trusted. */
    if (!g_slist_find(live_artworks, art)) {
        LS_LOG("cover arrived for a panel that is gone; dropping it");
        free(ready->filename);
        free(ready);
        return G_SOURCE_REMOVE;
    }

    cairo_surface_t *surface = build_backdrop(ready->filename);
    if (surface) {
        if (art->previous) {
            cairo_surface_destroy(art->previous);
        }
        art->previous = art->current;
        art->current = surface;
        art->fade = 0.0;
        art->fade_start = g_get_monotonic_time();
    }
    free(ready->filename);
    free(ready);
    return G_SOURCE_REMOVE;
}

static void cover_callback(int error, ddb_cover_query_t *query,
                           ddb_cover_info_t *cover) {
    LsArtwork *art = query->user_data;

    if (!error && cover && cover->cover_found && cover->image_filename) {
        LsCoverReady *ready = calloc(1, sizeof(LsCoverReady));
        if (ready) {
            ready->art = art;
            ready->filename = strdup(cover->image_filename);
            /* The artwork plugin answers on its own thread; building a
             * cairo surface and touching the panel both belong on the GTK
             * main loop. */
            g_idle_add(install_backdrop, ready);
        }
    }

    if (cover && artwork_plugin) {
        artwork_plugin->cover_info_release(cover);
    }
    if (query->track) {
        deadbeef->pl_item_unref(query->track);
    }
    free(query);
}

void ls_artwork_load(LsArtwork *art, DB_playItem_t *track) {
    if (!art || !track || !artwork_plugin) {
        return;
    }

    deadbeef->pl_lock();
    const char *uri = deadbeef->pl_find_meta(track, ":URI");
    char *path = uri ? strdup(uri) : NULL;
    deadbeef->pl_unlock();
    if (!path) {
        return;
    }
    if (art->path && strcmp(art->path, path) == 0) {
        free(path); /* same track — the cached blur still stands */
        return;
    }
    free(art->path);
    art->path = path;

    /* One source id per panel, allocated on its first load: it is what
     * makes this panel's queries cancellable as a group when the panel
     * goes away. Allocation also marks the panel as a live destination
     * for replies. */
    if (!g_slist_find(live_artworks, art)) {
        live_artworks = g_slist_prepend(live_artworks, art);
        if (artwork_plugin->allocate_source_id) {
            art->source_id = artwork_plugin->allocate_source_id();
        }
    }

    ddb_cover_query_t *query = calloc(1, sizeof(ddb_cover_query_t));
    if (!query) {
        return;
    }
    query->_size = sizeof(ddb_cover_query_t);
    query->user_data = art;
    query->flags = 0;
    query->track = track;
    query->source_id = art->source_id;
    deadbeef->pl_item_ref(track); /* released in the callback */
    artwork_plugin->cover_get(query, cover_callback);
}

/* -- drawing ---------------------------------------------------------- */

/* Paint `surface` scaled to cover the panel, centre-cropped. Covers are
 * square and the panel usually is not, so "fit" would letterbox and
 * "stretch" would smear; cover-and-crop is what reads as a backdrop. */
static void paint_cover(cairo_t *cr, cairo_surface_t *surface, int width,
                        int height, double alpha) {
    if (!surface || alpha <= 0.001) {
        return;
    }
    const double sw = cairo_image_surface_get_width(surface);
    const double sh = cairo_image_surface_get_height(surface);
    const double scale = MAX(width / sw, height / sh);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_translate(cr, (width - sw * scale) / 2.0, (height - sh * scale) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

void ls_artwork_draw(LsArtwork *art, cairo_t *cr, int width, int height) {
    /* An opaque ground under everything. The panel is drawn from scratch
     * each frame, so unlike the Qt version there is no way for a moved or
     * destroyed line to leave its pixels behind — this fill is what makes
     * that true. */
    cairo_set_source_rgba(cr, LS_COLOUR_BACKDROP);
    cairo_paint(cr);

    if (art) {
        paint_cover(cr, art->previous, width, height, 1.0 - art->fade);
        paint_cover(cr, art->current, width, height, art->fade);
    }

    /* Rate-limited to once a second, but fired at once when either
     * surface is swapped: a cross-fade is over in 620ms, so a purely
     * periodic report would usually miss the swap it exists to catch. */
    if (art && blur_debug()) {
        const gint64 now = g_get_monotonic_time();
        const int swapped = art->current != art->debug_current ||
                            art->previous != art->debug_previous;
        if (swapped || now - art->debug_logged > G_USEC_PER_SEC) {
            int cur[3], prev[3];
            surface_spread(art->current, cur);
            surface_spread(art->previous, prev);
            LS_LOG("blur draw: panel %dx%d fade=%.3f current=%p spread "
                   "B=%d G=%d R=%d, previous=%p spread B=%d G=%d R=%d%s",
                   width, height, art->fade, (void *)art->current, cur[0],
                   cur[1], cur[2], (void *)art->previous, prev[0], prev[1],
                   prev[2], swapped ? " (swapped)" : "");
            art->debug_logged = now;
            art->debug_current = art->current;
            art->debug_previous = art->previous;
        }
    }

    /* Scrim, so lyrics stay readable over bright covers. */
    cairo_set_source_rgba(cr, 0.047, 0.055, 0.075, LS_SCRIM_ALPHA);
    cairo_paint(cr);
}

int ls_artwork_advance(LsArtwork *art) {
    if (!art || art->fade >= 1.0 || !art->fade_start) {
        return 0;
    }
    const double elapsed =
        (double)(g_get_monotonic_time() - art->fade_start) / 1000.0;
    art->fade = ls_ease_out_cubic(elapsed / LS_BACKDROP_FADE_MS);
    if (elapsed >= LS_BACKDROP_FADE_MS) {
        art->fade = 1.0;
        art->fade_start = 0;
        if (art->previous) {
            cairo_surface_destroy(art->previous);
            art->previous = NULL;
        }
        return 0;
    }
    return 1;
}

void ls_artwork_invalidate(LsArtwork *art) {
    /* Forget which track the cached surface was built for, so the next
     * load rebuilds it. Used when the blur radius changes: the blur is
     * baked into the surface, so nothing short of rebuilding it will show
     * a new radius. */
    if (art) {
        free(art->path);
        art->path = NULL;
    }
}

void ls_artwork_clear(LsArtwork *art) {
    if (!art) {
        return;
    }
    /* Before anything is freed: stop the replies that would land in it.
     * Cancelling first and de-listing second is the order that leaves no
     * gap — a query cancelled after de-listing would have its reply
     * dropped anyway, but one still running after the memory is freed is
     * a write into it. */
    if (artwork_plugin && art->source_id &&
        artwork_plugin->cancel_queries_with_source_id) {
        artwork_plugin->cancel_queries_with_source_id(art->source_id);
    }
    live_artworks = g_slist_remove(live_artworks, art);

    if (art->current) {
        cairo_surface_destroy(art->current);
    }
    if (art->previous) {
        cairo_surface_destroy(art->previous);
    }
    free(art->path);
    memset(art, 0, sizeof(*art));
}
