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
    double r, g, b, y, sat;
} Sample;

static int by_saturation_desc(const void *a, const void *b) {
    const double sa = ((const Sample *)a)->sat;
    const double sb = ((const Sample *)b)->sat;
    return (sa < sb) - (sa > sb);
}

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

/* -- the highlight boost ---------------------------------------------- */

/* How much more saturated the highlight is made. Applied to the highlight
 * only: the mid and dark tones are the backdrop of the bar and should stay
 * as the cover actually is, or the whole panel drifts away from the art. */
#define HI_SATURATION_BOOST 1.45

/* How much of the *remaining* headroom to climb in brightness. Headroom
 * rather than a flat multiplier is the whole trick: a tone already at 0.93
 * has almost nothing left to give and barely moves, while a muted one at
 * 0.44 lifts properly. That is what stops a bright cover blowing out. */
#define HI_VALUE_LIFT 0.38

/* Never quite 1.0. A highlight at full value with low saturation is white,
 * and white bars say nothing about the album. */
#define HI_VALUE_CEILING 0.96

/* The ceiling for a fully desaturated highlight. Light enough to stay
 * clearly above the mid tone, dark enough not to read as white. */
#define HI_VALUE_FLOOR_CEILING 0.82

static void rgb_to_hsv(const double rgb[3], double *h, double *s, double *v) {
    const double r = rgb[0], g = rgb[1], b = rgb[2];
    const double max = MAX(r, MAX(g, b));
    const double min = MIN(r, MIN(g, b));
    const double d = max - min;
    *v = max;
    *s = (max <= 0.0) ? 0.0 : d / max;
    if (d <= 0.0) {
        *h = 0.0;
        return;
    }
    if (max == r) {
        *h = 60.0 * fmod((g - b) / d, 6.0);
    } else if (max == g) {
        *h = 60.0 * ((b - r) / d + 2.0);
    } else {
        *h = 60.0 * ((r - g) / d + 4.0);
    }
    if (*h < 0.0) {
        *h += 360.0;
    }
}

static void hsv_to_rgb(double h, double s, double v, double rgb[3]) {
    const double c = v * s;
    const double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    const double m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60)       { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

/* Make the highlight read as a colour rather than as "the brightest
 * average pixel", which on a desaturated cover is just pale grey.
 *
 * The brightness lift is scaled by saturation as well as by headroom. A
 * grey tone pushed up in value only ever becomes a paler grey — closer to
 * white, further from the album — so it is deliberately lifted least. A
 * saturated tone has somewhere to go, so it gets the full lift. */
static void punch_up(double rgb[3]) {
    double h, s, v;
    rgb_to_hsv(rgb, &h, &s, &v);

    const double boosted_s = MIN(1.0, s * HI_SATURATION_BOOST);
    const double lift = HI_VALUE_LIFT * (0.35 + 0.65 * boosted_s);
    double boosted_v = v + (1.0 - v) * lift;

    /* The ceiling tightens as saturation falls. A saturated tone can sit
     * near the top and still be obviously a colour; a desaturated one at
     * the same value is just white, so it is held lower. This is what
     * keeps a monochrome cover's highlight a light grey rather than a
     * blown-out slab. */
    const double ceiling =
        HI_VALUE_FLOOR_CEILING +
        (HI_VALUE_CEILING - HI_VALUE_FLOOR_CEILING) * boosted_s;
    if (boosted_v > ceiling) {
        boosted_v = ceiling;
    }
    hsv_to_rgb(h, boosted_s, boosted_v, rgb);
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
            const double mx = MAX(s->r, MAX(s->g, s->b));
            const double mn = MIN(s->r, MIN(s->g, s->b));
            s->sat = (mx <= 0.0) ? 0.0 : (mx - mn) / mx;
        }
    }
    g_object_unref(grid);

    if (count < 8) {
        return;
    }

    qsort(samples, count, sizeof(Sample), by_luminance);
    band_average(samples, count, 0.04, 0.18, out->lo);
    band_average(samples, count, 0.45, 0.55, out->mid);
    /* The highlight is chosen in two stages: brightest first, then most
     * colourful among those.
     *
     * Brightness alone is not enough, and picking a narrower, higher
     * percentile actually made it worse — measured across a library, four
     * covers came back at 0.96/0.96/0.96, which is flat white. Plenty of
     * covers have white text, a white border or a bright sky, and those
     * pixels win any contest that only asks "which is brightest" while
     * saying nothing whatsoever about the album.
     *
     * So: take the top third by luminance as candidates, then average the
     * most saturated slice of *those*. On a cover with colour, that finds
     * the bright colour rather than the white. On a genuinely monochrome
     * cover every candidate has zero saturation, the sort is a no-op, and
     * the result is the bright grey it should be. */
    const int bright_from = (int)(count * 0.66);
    int bright_count = count - bright_from;
    if (bright_count < 4) {
        bright_count = MIN(count, 4);
    }
    Sample bright[GRID * GRID];
    memcpy(bright, samples + (count - bright_count),
           (size_t)bright_count * sizeof(Sample));
    qsort(bright, bright_count, sizeof(Sample), by_saturation_desc);
    band_average(bright, bright_count, 0.0, 0.34, out->hi);
    punch_up(out->hi);
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
