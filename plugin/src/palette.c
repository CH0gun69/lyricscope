#include "palette.h"

#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The grid the cover is reduced to before anything is measured. 256
 * pixels is enough to be representative and small enough that the whole
 * extraction is lost in the noise of the track change that triggered it. */
#define GRID 16

/* Rec.709 luminance — the weights that match how bright a colour actually
 * looks, rather than treating R, G and B as equally bright. */
static double luminance(double r, double g, double b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

typedef struct {
    double r, g, b, y;
} Sample;

static int by_luminance(const void *a, const void *b) {
    const double ya = ((const Sample *)a)->y;
    const double yb = ((const Sample *)b)->y;
    return (ya > yb) - (ya < yb);
}

/* Average a percentile band of the luminance-sorted samples.
 *
 * Bands, not extremes: the single brightest pixel on a cover is very often
 * a specular highlight or a bit of white text, and the single darkest is
 * usually pure black border. Either would hand back a colour that appears
 * nowhere a viewer would say the cover "is". Averaging a slice well inside
 * each end gives a tone that reads as belonging to the art. */
static void band_average(const Sample *samples, int count, double from,
                         double to, double out[3]) {
    int start = (int)(from * count);
    int end = (int)(to * count);
    if (start < 0) start = 0;
    if (end > count) end = count;
    if (end <= start) {
        start = count / 2;
        end = start + 1;
        if (end > count) {
            out[0] = out[1] = out[2] = 0.5;
            return;
        }
    }
    double r = 0, g = 0, b = 0;
    for (int i = start; i < end; i++) {
        r += samples[i].r;
        g += samples[i].g;
        b += samples[i].b;
    }
    const double n = end - start;
    out[0] = r / n;
    out[1] = g / n;
    out[2] = b / n;
}

void ls_palette_from_pixbuf(GdkPixbuf *source, LsPalette *out) {
    memset(out, 0, sizeof(*out));
    if (!source) {
        return;
    }

    GdkPixbuf *grid =
        gdk_pixbuf_scale_simple(source, GRID, GRID, GDK_INTERP_BILINEAR);
    if (!grid) {
        return;
    }

    const int stride = gdk_pixbuf_get_rowstride(grid);
    const int channels = gdk_pixbuf_get_n_channels(grid);
    const guchar *pixels = gdk_pixbuf_get_pixels(grid);
    const int has_alpha = gdk_pixbuf_get_has_alpha(grid);

    Sample samples[GRID * GRID];
    int count = 0;
    for (int y = 0; y < GRID; y++) {
        const guchar *row = pixels + (gsize)y * stride;
        for (int x = 0; x < GRID; x++) {
            const guchar *px = row + (gsize)x * channels;
            /* A transparent pixel has no colour to contribute; including
             * it would drag every tone toward whatever happened to be in
             * the unused bytes. */
            if (has_alpha && px[3] < 128) {
                continue;
            }
            Sample *s = &samples[count++];
            s->r = px[0] / 255.0;
            s->g = px[1] / 255.0;
            s->b = px[2] / 255.0;
            s->y = luminance(s->r, s->g, s->b);
        }
    }
    g_object_unref(grid);

    if (count < 8) {
        return;
    }

    qsort(samples, count, sizeof(Sample), by_luminance);
    band_average(samples, count, 0.04, 0.18, out->lo);
    band_average(samples, count, 0.45, 0.55, out->mid);
    band_average(samples, count, 0.82, 0.96, out->hi);
    out->valid = 1;
}

/* -- one-entry cache --------------------------------------------------- */

static char *cached_path;
static LsPalette cached_palette;

void ls_palette_cache_put(const char *path, const LsPalette *palette) {
    if (!path || !palette || !palette->valid) {
        return;
    }
    free(cached_path);
    cached_path = strdup(path);
    cached_palette = *palette;
    LS_LOG("palette for %s: lo=%.2f/%.2f/%.2f mid=%.2f/%.2f/%.2f "
           "hi=%.2f/%.2f/%.2f",
           path, palette->lo[0], palette->lo[1], palette->lo[2],
           palette->mid[0], palette->mid[1], palette->mid[2], palette->hi[0],
           palette->hi[1], palette->hi[2]);
}

int ls_palette_cache_get(const char *path, LsPalette *out) {
    if (!path || !cached_path || strcmp(path, cached_path) != 0 ||
        !cached_palette.valid) {
        return 0;
    }
    *out = cached_palette;
    return 1;
}
