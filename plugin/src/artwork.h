#ifndef LS_ARTWORK_H
#define LS_ARTWORK_H

#include <deadbeef/deadbeef.h>
#include <gtk/gtk.h>

/* Blurred cover art behind the lyrics, cross-fading on track change.
 *
 * The blur is built once per track, never per frame: it costs tens of
 * milliseconds, which is several dropped frames if repeated. It is also
 * built at a small fixed size and scaled up at draw time — the result is
 * blurred, so upscaling costs it nothing, and a panel resize does not
 * need a rebuild. */
typedef struct {
    cairo_surface_t *current;
    cairo_surface_t *previous;
    double fade; /* 0..1 across the cross-fade */
    gint64 fade_start;
    char *path;  /* the track this was built for */
    int64_t source_id;
} LsArtwork;

/* Ask DeaDBeeF's artwork plugin for this track's cover and build the
 * backdrop from it. Asynchronous — the artwork plugin answers on its own
 * thread — and a no-op if the track has not actually changed.
 *
 * Takes the playlist item rather than a path because that is what the
 * artwork API wants, and going through it is what gets embedded covers,
 * folder images and the player's own cache for free. */
void ls_artwork_load(LsArtwork *art, DB_playItem_t *track);

/* Look up DeaDBeeF's artwork plugin once, at plugin start. */
void ls_artwork_init(void);

/* Drop the "already built for this track" memo so the next load rebuilds
 * the surface. The blur is baked in, so a radius change needs this. */
void ls_artwork_invalidate(LsArtwork *art);

void ls_artwork_draw(LsArtwork *art, cairo_t *cr, int width, int height);

/* Advance the cross-fade; non-zero while it is still moving. */
int ls_artwork_advance(LsArtwork *art);

void ls_artwork_clear(LsArtwork *art);

#endif /* LS_ARTWORK_H */
