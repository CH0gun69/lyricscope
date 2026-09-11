/* The only place this plugin talks to Python.
 *
 * Phase 0 measured the alternative honestly: resolving lyrics means the
 * sidecar/tag/central-folder search, the LRC timestamp grammar, the
 * synced-beats-unsynced preference and the filename normalisation — all of
 * which already exist, tested, in core/. Reimplementing that in C would be
 * a straight rewrite of working code with no user-visible gain.
 *
 * So Python answers exactly one question, once per track change: "given
 * this path, artist and title, what are the lines and their timestamps?"
 * Everything after that — matching position to a line, laying out, drawing,
 * animating — is C, and the 60fps path never enters the interpreter.
 */

#ifndef LS_PYBRIDGE_H
#define LS_PYBRIDGE_H

typedef struct {
    char *text;
    double time; /* seconds; negative means this line carries no timestamp */
} LsLine;

typedef struct {
    LsLine *lines;
    int count;
    int synced;
    char *source; /* human-readable, shown in the panel's status line */
} LsLyrics;

/* Start the interpreter and import core.resolver. Safe to call twice.
 * Returns 0 on success. */
int ls_py_init(void);

/* Resolve one track. `out` is overwritten and owned by the caller, who
 * must pass it to ls_lyrics_clear(). Returns 0 on success; on failure
 * `out` is left empty rather than untouched. */
int ls_py_resolve(const char *path, const char *artist, const char *title,
                  LsLyrics *out);

void ls_lyrics_clear(LsLyrics *lyrics);

/* Index of the line active at `position`, or -1 before the first
 * timestamp. Pure C — this runs every frame. */
int ls_lyrics_index_at(const LsLyrics *lyrics, double position);

#endif /* LS_PYBRIDGE_H */
