/* Three tones pulled out of a track's cover art.
 *
 * Deliberately not a general colour-quantisation library: the spectrum
 * needs a dark, a mid and a light tone that obviously belong to the cover,
 * and a 16x16 downsample gives that for a few microseconds of work. The
 * expensive, correct answers (median cut, k-means) would buy accuracy
 * nobody can see behind a set of moving bars.
 */

#ifndef LS_PALETTE_H
#define LS_PALETTE_H

#include <gtk/gtk.h>

typedef struct {
    double lo[3];  /* darkest band, r/g/b in 0..1 */
    double mid[3]; /* middle band */
    double hi[3];  /* lightest band */
    int valid;
} LsPalette;

/* Extract from an already-decoded pixbuf. The caller's pixbuf is only
 * read. */
void ls_palette_from_pixbuf(GdkPixbuf *source, LsPalette *out);

/* A one-entry cache keyed by track path.
 *
 * One entry rather than a map because only the playing track's colours are
 * ever wanted, and a map would need an eviction policy to avoid growing
 * for the length of a listening session.
 *
 * The point of the cache is that the lyrics panel's backdrop build already
 * decodes and downsamples the same cover; it drops the result in here on
 * its way past, so the spectrum panel usually finds the colours already
 * waiting and never touches the image at all. */
void ls_palette_cache_put(const char *path, const LsPalette *palette);
int ls_palette_cache_get(const char *path, LsPalette *out);

#endif /* LS_PALETTE_H */
