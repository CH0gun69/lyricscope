/* The docked lyric panel: one GtkDrawingArea, everything drawn by hand.
 *
 * The PySide6 version this replaces used one widget per lyric line,
 * because Qt can rescale a widget cheaply and re-laying out a whole
 * document every tick is where that kind of view goes slow. Cairo is
 * immediate mode, so the same reasoning inverts: there is nothing to keep
 * alive between frames, and a line is just a Pango layout drawn under a
 * transform. The entire class of bug that came with per-line widgets —
 * orphaned children surviving a track change — cannot be expressed here.
 *
 * What *can* survive a track change is stale state: the lines array, the
 * per-line animation states and the cached layouts. So the teardown is
 * still hard and synchronous, and there is still an invariant check on it
 * (ls_panel_debug_state), because the failure mode reappearing in a new
 * form was the thing to watch for rather than assume away.
 */

#include "panel.h"

#include "animations.h"
#include "artwork.h"
#include "config_dialog.h"
#include "log.h"
#include "pybridge.h"
#include "settings.h"
#include "theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern DB_functions_t *deadbeef;
/* Needed for w_get_design_mode(), so right-click can tell Design Mode's
 * context menu apart from the panel's own. */
extern ddb_gtkui_t *gtkui_plugin;

/* Per-line animation and layout. The Pango layout is cached because
 * rebuilding one per line per frame is the obvious way to make this
 * panel slow, and text only reflows when the width or the lyrics change. */
typedef struct {
    PangoLayout *layout;
    double y;      /* top, in column space */
    double height; /* at full scale */

    double scale;      /* animated */
    double scale_from;
    double scale_to;
    gint64 scale_start; /* µs, 0 when not animating */
} LsLineState;

/* Distinct from -1, which is a real state: before the first timestamp
 * there is genuinely no current line, and that is not the same as never
 * having had one. Only the latter suppresses the expand animation. */
#define LS_NO_CURRENT (-2)

struct LsPanel {
    ddb_gtkui_widget_t base;
    /* The container is what gtkui gets handed, and the drawing area sits
     * inside it. Handing gtkui the drawing area directly looks tidier and
     * silently breaks clicking: w_override_signals installs Design Mode's
     * own button handling on whatever widget it is given, so the panel's
     * own press handler never runs. Lyricbar has the same shape for the
     * same reason — a container for gtkui, a child for the content. */
    GtkWidget *container;
    GtkWidget *area;

    LsLyrics lyrics;
    LsLineState *state;
    int current;
    char *track_path;

    double scroll;
    double scroll_from;
    double scroll_to;
    gint64 scroll_start;

    double fade;
    gint64 fade_start;

    LsArtwork art;

    PangoFontDescription *font;
    int layout_width; /* width the cached layouts were measured at */
    int layout_valid;

    guint tick_id;
    char *message; /* shown instead of lyrics: "no lyrics found", errors */
};

static gint64 now_us(void) { return g_get_monotonic_time(); }

static double ms_since(gint64 start) {
    return start ? (double)(now_us() - start) / 1000.0 : 1e9;
}

/* -- teardown --------------------------------------------------------- */

static void clear_line_state(LsPanel *panel) {
    if (panel->state) {
        for (int i = 0; i < panel->lyrics.count; i++) {
            if (panel->state[i].layout) {
                g_object_unref(panel->state[i].layout);
            }
        }
        free(panel->state);
        panel->state = NULL;
    }
}

/* Drop everything belonging to the outgoing track, synchronously.
 *
 * Nothing here is deferred to an idle callback or an animation-finished
 * signal. The Qt version's stacking bug came from exactly that kind of
 * deferral, and while Cairo cannot orphan a widget, a half-freed state
 * array during a fast skip would be worse than a visual artefact. */
static void teardown(LsPanel *panel) {
    clear_line_state(panel);
    ls_lyrics_clear(&panel->lyrics);
    panel->current = LS_NO_CURRENT;
    panel->scroll = panel->scroll_from = panel->scroll_to = 0.0;
    panel->scroll_start = 0;
    panel->layout_valid = 0;
    g_clear_pointer(&panel->message, free);
}

/* Post-rebuild invariant, logged rather than asserted so a mismatch shows
 * up in the player's log during real use instead of only under a test. */
void ls_panel_debug_state(LsPanel *panel, const char *when) {
    int layouts = 0;
    for (int i = 0; i < panel->lyrics.count; i++) {
        if (panel->state && panel->state[i].layout) {
            layouts++;
        }
    }
    LS_LOG("state after %s: lines=%d line_states=%s cached_layouts=%d "
           "current=%d synced=%d source=%s",
           when, panel->lyrics.count, panel->state ? "yes" : "none", layouts,
           panel->current, panel->lyrics.synced,
           panel->lyrics.source ? panel->lyrics.source : "(none)");
}

/* -- layout ----------------------------------------------------------- */

/* Re-stamp the configured size onto the font and drop the cached layouts,
 * which were measured at the old one. */
static void apply_text_size(LsPanel *panel) {
    pango_font_description_set_absolute_size(panel->font,
                                             ls_settings.text_px * PANGO_SCALE);
    panel->layout_valid = 0;
}

static void build_layouts(LsPanel *panel, int width) {
    if (!panel->state || panel->lyrics.count == 0) {
        panel->layout_valid = 1;
        panel->layout_width = width;
        return;
    }

    const int wrap = width - (int)(2 * LS_SIDE_PADDING);
    double y = 0.0;
    for (int i = 0; i < panel->lyrics.count; i++) {
        LsLineState *line = &panel->state[i];
        if (line->layout) {
            g_object_unref(line->layout);
        }
        line->layout = gtk_widget_create_pango_layout(panel->area, NULL);
        pango_layout_set_font_description(line->layout, panel->font);
        pango_layout_set_width(line->layout, MAX(wrap, 1) * PANGO_SCALE);
        pango_layout_set_wrap(line->layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(line->layout, ls_settings_pango_align());
        const char *text = panel->lyrics.lines[i].text;
        pango_layout_set_text(line->layout, (text && *text) ? text : " ", -1);

        int w, h;
        pango_layout_get_pixel_size(line->layout, &w, &h);
        line->y = y;
        line->height = h;
        y += h + LS_LINE_SPACING;
    }
    panel->layout_valid = 1;
    panel->layout_width = width;
}

static void ensure_layout(LsPanel *panel) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(panel->area, &alloc);
    if (!panel->layout_valid || panel->layout_width != alloc.width) {
        build_layouts(panel, alloc.width);
    }
}

/* Where the column has to sit for `index` to land on the focus line. */
static double scroll_for(LsPanel *panel, int index) {
    if (!panel->state || index < 0 || index >= panel->lyrics.count) {
        return panel->scroll_to;
    }
    GtkAllocation alloc;
    gtk_widget_get_allocation(panel->area, &alloc);
    const LsLineState *line = &panel->state[index];
    return line->y + line->height / 2.0 - alloc.height * LS_FOCUS;
}

/* -- current line ----------------------------------------------------- */

static void start_scale(LsLineState *line, double target, int animate) {
    if (!animate) {
        line->scale = line->scale_from = line->scale_to = target;
        line->scale_start = 0;
        return;
    }
    if (fabs(line->scale_to - target) < 0.0005) {
        return;
    }
    line->scale_from = line->scale;
    line->scale_to = target;
    line->scale_start = now_us();
}

static void set_current(LsPanel *panel, int index, int animate) {
    if (index == panel->current || !panel->state) {
        return;
    }
    const int previous = panel->current;
    panel->current = index;

    /* The first highlight after a rebuild has nothing to animate *from*:
     * every line is still at its resting scale, so tweening would show the
     * unfocused arrangement first and then grow into the right one — a
     * visible pop exactly when a track's lyrics appear. Snap instead, so
     * the first painted frame is already correct. */
    if (previous == LS_NO_CURRENT) {
        animate = 0;
    }

    for (int i = 0; i < panel->lyrics.count; i++) {
        const double target =
            (i == index) ? LS_CURRENT_SCALE : LS_NORMAL_SCALE;
        /* Only the two lines actually in play animate; everything else is
         * snapped, in case a previous transition was cut off part-way. */
        const int tween = animate && (i == index || i == previous);
        start_scale(&panel->state[i], target, tween);
    }

    const double target = scroll_for(panel, index);
    if (animate) {
        panel->scroll_from = panel->scroll;
        panel->scroll_to = target;
        panel->scroll_start = now_us();
    } else {
        panel->scroll = panel->scroll_from = panel->scroll_to = target;
        panel->scroll_start = 0;
    }
}

/* -- track change ----------------------------------------------------- */

void ls_panel_set_message(LsPanel *panel, const char *message) {
    teardown(panel);
    panel->message = message ? strdup(message) : NULL;
    gtk_widget_queue_draw(panel->area);
}

static void load_track(LsPanel *panel, const char *path, const char *artist,
                       const char *title) {
    teardown(panel);

    free(panel->track_path);
    panel->track_path = path ? strdup(path) : NULL;

    if (!path || !*path) {
        panel->message = strdup("Nothing playing");
        gtk_widget_queue_draw(panel->area);
        return;
    }

    if (ls_py_resolve(path, artist, title, &panel->lyrics) != 0 ||
        panel->lyrics.count == 0) {
        ls_lyrics_clear(&panel->lyrics);
        panel->message = strdup("No lyrics found");
        gtk_widget_queue_draw(panel->area);
        return;
    }

    panel->state = calloc((size_t)panel->lyrics.count, sizeof(LsLineState));
    if (!panel->state) {
        ls_lyrics_clear(&panel->lyrics);
        return;
    }
    for (int i = 0; i < panel->lyrics.count; i++) {
        /* Unsynced lyrics have no current line, so they rest at full scale
         * rather than leaving the whole column shrunk for nothing. */
        panel->state[i].scale = panel->lyrics.synced ? LS_NORMAL_SCALE
                                                     : LS_CURRENT_SCALE;
        panel->state[i].scale_to = panel->state[i].scale;
    }

    panel->fade = 0.0;
    panel->fade_start = now_us();
    ls_panel_debug_state(panel, "track change");
    gtk_widget_queue_draw(panel->area);
}

/* -- drawing ---------------------------------------------------------- */

static void draw_centred_message(LsPanel *panel, cairo_t *cr, int width,
                                 int height, const char *text) {
    PangoLayout *layout = gtk_widget_create_pango_layout(panel->area, text);
    pango_layout_set_font_description(layout, panel->font);
    pango_layout_set_width(layout, MAX(width - 96, 1) * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    cairo_set_source_rgba(cr, LS_COLOUR_PLACEHOLDER);
    cairo_move_to(cr, (width - w) / 2.0, (height - h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    LsPanel *panel = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    ls_artwork_draw(&panel->art, cr, alloc.width, alloc.height);

    if (panel->message) {
        draw_centred_message(panel, cr, alloc.width, alloc.height,
                             panel->message);
        return FALSE;
    }
    if (!panel->state || panel->lyrics.count == 0) {
        return FALSE;
    }

    ensure_layout(panel);

    /* One fade for the whole column rather than one per line. The staggered
     * per-line version looked good and was where the widget-lifetime bugs
     * lived; this is a single group opacity and cannot leave anything
     * behind. */
    const double fade = panel->fade;
    if (fade < 0.999) {
        cairo_push_group(cr);
    }

    for (int i = 0; i < panel->lyrics.count; i++) {
        const LsLineState *line = &panel->state[i];
        const double top = line->y - panel->scroll;

        /* Immediate mode means offscreen lines cost nothing if they are
         * simply not drawn — there is no widget to keep in sync. */
        if (top + line->height < -LS_LINE_PX || top > alloc.height + LS_LINE_PX) {
            continue;
        }

        if (!panel->lyrics.synced) {
            cairo_set_source_rgba(cr, LS_COLOUR_PLAIN);
        } else if (i == panel->current) {
            double ar, ag, ab;
            ls_settings_accent_rgb(&ar, &ag, &ab);
            cairo_set_source_rgba(cr, ar, ag, ab, 1.0);
        } else if (i < panel->current) {
            cairo_set_source_rgba(cr, LS_COLOUR_PAST);
        } else {
            cairo_set_source_rgba(cr, LS_COLOUR_FUTURE);
        }

        cairo_save(cr);
        /* Scale about the line's centre so it grows and shrinks in place
         * rather than drifting toward a corner.
         *
         * This is a transform on the glyph outlines, NOT a font size
         * change, and that is the whole point. The Qt version proved that
         * animating the font instead quantises: setPixelSize takes an int
         * and setPointSizeF is rounded back to whole pixels by the font
         * engine, so 0.80 -> 1.00 of a 26px line yields six distinct sizes
         * and a step every other frame. Pango would quantise the same way.
         * Transforming keeps the interpolation continuous, keeps the
         * layout metrics constant so nothing below a growing line moves,
         * and stays vector-sharp because the outlines are what get
         * transformed. */
        /* Scale about the aligned edge, not always the panel centre.
         * Centring the transform under left-aligned text would slide the
         * line sideways as it grew, because its own centre is nowhere near
         * the panel's; anchoring on the edge the text is aligned to keeps
         * that edge still and lets the line grow inward, which is what
         * reads as the line getting bigger rather than moving. */
        double cx;
        switch (ls_settings.alignment) {
        case LS_ALIGN_LEFT:
            cx = LS_SIDE_PADDING;
            break;
        case LS_ALIGN_RIGHT:
            cx = alloc.width - LS_SIDE_PADDING;
            break;
        default:
            cx = alloc.width / 2.0;
            break;
        }
        const double cy = top + line->height / 2.0;
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, line->scale, line->scale);
        cairo_translate(cr, -cx, -cy);

        cairo_move_to(cr, LS_SIDE_PADDING, top);
        pango_cairo_show_layout(cr, line->layout);
        cairo_restore(cr);
    }

    if (fade < 0.999) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, fade);
    }
    return FALSE;
}

/* -- animation -------------------------------------------------------- */

/* Advance every animated value; returns non-zero while anything moves. */
static int advance(LsPanel *panel) {
    int moving = 0;

    if (panel->fade < 1.0) {
        const double t = ms_since(panel->fade_start) / LS_LYRICS_FADE_MS;
        panel->fade = ls_ease_out_cubic(t);
        if (t < 1.0) {
            moving = 1;
        } else {
            panel->fade = 1.0;
        }
    }

    if (panel->scroll_start) {
        const double elapsed = ms_since(panel->scroll_start);
        panel->scroll = ls_interp(panel->scroll_from, panel->scroll_to, elapsed,
                                  LS_SCROLL_MS);
        if (elapsed >= LS_SCROLL_MS) {
            panel->scroll = panel->scroll_to;
            panel->scroll_start = 0;
        } else {
            moving = 1;
        }
    }

    for (int i = 0; i < panel->lyrics.count; i++) {
        LsLineState *line = &panel->state[i];
        if (!line->scale_start) {
            continue;
        }
        const double elapsed = ms_since(line->scale_start);
        const double duration = (double)ls_settings.transition_ms;
        line->scale =
            ls_interp(line->scale_from, line->scale_to, elapsed, duration);
        if (i == panel->current && getenv("LS_DEBUG_SCALE")) {
            LS_LOG("scale %.5f", line->scale);
        }
        if (elapsed >= duration) {
            line->scale = line->scale_to;
            line->scale_start = 0;
        } else {
            moving = 1;
        }
    }

    if (ls_artwork_advance(&panel->art)) {
        moving = 1;
    }
    return moving;
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data) {
    (void)clock;
    LsPanel *panel = user_data;

    /* Position comes from the streamer directly — in-process, no IPC, no
     * polling a subprocess twice a second and interpolating between
     * answers the way the standalone app had to. */
    if (panel->lyrics.count > 0 && panel->lyrics.synced) {
        const double position = deadbeef->streamer_get_playpos();
        const int index = ls_lyrics_index_at(&panel->lyrics, position);
        if (index != panel->current) {
            ensure_layout(panel);
            set_current(panel, index, TRUE);
        }
    }

    if (advance(panel)) {
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* -- clicking --------------------------------------------------------- */

/* Defined below with the rest of the settings plumbing; declared here
 * because the press handler is what opens it. */
static void show_context_menu(LsPanel *panel, GdkEventButton *event);


static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                gpointer user_data) {
    (void)widget;
    LsPanel *panel = user_data;

    if (event->button == 3) {
        /* Design Mode owns right-click while it is on — that menu is how a
         * widget gets moved, replaced or deleted, and quietly replacing it
         * with a settings entry would strand the panel in the layout. Its
         * handler is on the container, so declining here is enough to let
         * the event reach it. */
        if (gtkui_plugin && gtkui_plugin->w_get_design_mode()) {
            return FALSE;
        }
        show_context_menu(panel, event);
        return TRUE;
    }

    if (event->button != 1 || !panel->state || !panel->lyrics.synced) {
        return FALSE;
    }

    const double column_y = event->y + panel->scroll;
    for (int i = 0; i < panel->lyrics.count; i++) {
        const LsLineState *line = &panel->state[i];
        /* The gap between lines counts as part of the line above it.
         * Clicks that landed in the 16px gutters were the single most
         * common miss when this was tested in the Qt version. */
        const double bottom = line->y + line->height + LS_LINE_SPACING;
        if (column_y >= line->y && column_y < bottom) {
            const double when = panel->lyrics.lines[i].time;
            if (when < 0.0) {
                return FALSE;
            }
            /* Straight to the player: no MPRIS, no gdbus, no int64
             * marshalling trap, no `--` needed to stop a negative offset
             * being parsed as an option. In-process this is one call. */
            deadbeef->sendmessage(DB_EV_SEEK, 0, (uint32_t)(when * 1000.0), 0);
            LS_LOG("seek to %.2fs (line %d)", when, i);
            return TRUE;
        }
    }
    return FALSE;
}

/* -- settings ---------------------------------------------------------- */

/* Re-centre without animating after anything that changes line metrics:
 * every line moved, so tweening to the new position would read as a
 * scroll the user did not ask for. */
static void resettle(LsPanel *panel) {
    if (panel->current >= 0) {
        ensure_layout(panel);
        panel->scroll = panel->scroll_to = scroll_for(panel, panel->current);
        panel->scroll_start = 0;
    }
    gtk_widget_queue_draw(panel->area);
}

static void on_settings_changed(void *user_data) {
    LsPanel *panel = user_data;
    apply_text_size(panel); /* also invalidates the cached layouts */
    resettle(panel);
}

/* Separate from the above because the blur is baked into a cached surface:
 * a repaint would faithfully redraw the old radius. */
static void on_blur_changed(void *user_data) {
    LsPanel *panel = user_data;
    ls_artwork_invalidate(&panel->art);
    DB_playItem_t *track = deadbeef->streamer_get_playing_track_safe();
    if (track) {
        ls_artwork_load(&panel->art, track);
        deadbeef->pl_item_unref(track);
    }
    gtk_widget_queue_draw(panel->area);
}

static void on_settings_activate(GtkMenuItem *item, gpointer data) {
    (void)item;
    LsPanel *panel = data;
    ls_config_dialog_show(panel->area, on_settings_changed, on_blur_changed,
                          panel);
}

static void show_context_menu(LsPanel *panel, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_label("Settings\u2026");
    g_signal_connect(item, "activate", G_CALLBACK(on_settings_activate), panel);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    /* Built fresh per click and torn down when it closes; attaching it to
     * the panel would outlive the popup for no reason. */
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy),
                     NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static void on_size_allocate(GtkWidget *widget, GdkRectangle *alloc,
                             gpointer user_data) {
    (void)widget;
    (void)alloc;
    LsPanel *panel = user_data;
    panel->layout_valid = 0;
    /* Every line just moved, so re-centre without animating — this fires
     * while the pane divider is being dragged. */
    if (panel->current >= 0) {
        ensure_layout(panel);
        panel->scroll = panel->scroll_to = scroll_for(panel, panel->current);
        panel->scroll_start = 0;
    }
}

/* -- lifecycle -------------------------------------------------------- */

static void panel_destroy(ddb_gtkui_widget_t *widget) {
    LsPanel *panel = (LsPanel *)widget;
    if (panel->tick_id) {
        gtk_widget_remove_tick_callback(panel->area, panel->tick_id);
        panel->tick_id = 0;
    }
    teardown(panel);
    free(panel->track_path);
    panel->track_path = NULL;
    ls_artwork_clear(&panel->art);
    if (panel->font) {
        pango_font_description_free(panel->font);
        panel->font = NULL;
    }
}

/* DeaDBeeF delivers events on its message pump thread, so anything
 * touching GTK has to hop to the main loop first. */
typedef struct {
    LsPanel *panel;
    char *path;
    char *artist;
    char *title;
    DB_playItem_t *track; /* referenced; released here */
} LsTrackChange;

static gboolean apply_track_change(gpointer data) {
    LsTrackChange *change = data;
    load_track(change->panel, change->path, change->artist, change->title);
    if (change->track) {
        ls_artwork_load(&change->panel->art, change->track);
        deadbeef->pl_item_unref(change->track);
    }
    gtk_widget_queue_draw(change->panel->area);
    free(change->path);
    free(change->artist);
    free(change->title);
    free(change);
    return G_SOURCE_REMOVE;
}

static int panel_message(ddb_gtkui_widget_t *widget, uint32_t id,
                         uintptr_t ctx, uint32_t p1, uint32_t p2) {
    (void)p1;
    (void)p2;
    LsPanel *panel = (LsPanel *)widget;

    if (id == DB_EV_SONGSTARTED) {
        ddb_event_track_t *event = (ddb_event_track_t *)ctx;
        if (!event || !event->track) {
            return 0;
        }
        LsTrackChange *change = calloc(1, sizeof(LsTrackChange));
        if (!change) {
            return 0;
        }
        change->panel = panel;
        deadbeef->pl_lock();
        const char *uri = deadbeef->pl_find_meta(event->track, ":URI");
        const char *artist = deadbeef->pl_find_meta(event->track, "artist");
        const char *title = deadbeef->pl_find_meta(event->track, "title");
        change->path = uri ? strdup(uri) : NULL;
        change->artist = artist ? strdup(artist) : NULL;
        change->title = title ? strdup(title) : NULL;
        deadbeef->pl_unlock();
        /* Referenced because the idle callback runs later, by which time
         * the event's track may well have been freed. */
        change->track = event->track;
        deadbeef->pl_item_ref(change->track);
        g_idle_add(apply_track_change, change);
    } else if (id == DB_EV_STOP) {
        LsTrackChange *change = calloc(1, sizeof(LsTrackChange));
        if (change) {
            change->panel = panel;
            g_idle_add(apply_track_change, change);
        }
    }
    return 0;
}

LsPanel *ls_panel_new(void) {
    LsPanel *panel = calloc(1, sizeof(LsPanel));
    if (!panel) {
        return NULL;
    }
    panel->current = LS_NO_CURRENT;
    panel->fade = 1.0;

    /* An event box, not a plain box: it owns a GdkWindow, so Design Mode's
     * right-click still lands on something after the drawing area declines
     * an event it does not want. */
    panel->container = gtk_event_box_new();
    panel->area = gtk_drawing_area_new();
    gtk_widget_add_events(panel->area, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_can_focus(panel->area, TRUE);
    gtk_container_add(GTK_CONTAINER(panel->container), panel->area);

    /* Take the theme's font but fix the size here. An absolute size is in
     * device units, so it is exactly LS_LINE_PX and never rounds — and it
     * never changes afterwards, because scale is a transform. */
    PangoContext *context = gtk_widget_get_pango_context(panel->area);

    /* Turn off metric hinting for this panel's text.
     *
     * Scaling the cairo context is continuous, but hinted metrics are not:
     * with hinting on, each glyph's advance is rounded to a whole device
     * pixel *after* the transform, so a line growing from 0.80 to 1.00
     * re-snaps its letter spacing several times on the way and shimmers.
     * That is the same quantisation the Qt version hit at the font-size
     * level, one layer further down — worth disabling up front rather than
     * rediscovering. Slight hinting keeps the stems clean vertically. */
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(context, options);
    cairo_font_options_destroy(options);

    const PangoFontDescription *base = pango_context_get_font_description(context);
    panel->font = base ? pango_font_description_copy(base)
                       : pango_font_description_from_string("Sans");
    pango_font_description_set_weight(panel->font, PANGO_WEIGHT_MEDIUM);
    apply_text_size(panel);

    g_signal_connect(panel->area, "draw", G_CALLBACK(on_draw), panel);
    g_signal_connect(panel->area, "button-press-event",
                     G_CALLBACK(on_button_press), panel);
    g_signal_connect(panel->area, "size-allocate",
                     G_CALLBACK(on_size_allocate), panel);

    panel->tick_id =
        gtk_widget_add_tick_callback(panel->area, on_tick, panel, NULL);

    panel->base.widget = panel->container;
    panel->base.destroy = panel_destroy;
    panel->base.message = panel_message;

    panel->message = strdup("Waiting for playback");
    gtk_widget_show_all(panel->container);
    return panel;
}

ddb_gtkui_widget_t *ls_panel_as_widget(LsPanel *panel) {
    return &panel->base;
}

/* Pick up whatever is already playing, so a panel added mid-song fills in
 * immediately instead of waiting for the next track. */
void ls_panel_sync_now(LsPanel *panel) {
    DB_playItem_t *track = deadbeef->streamer_get_playing_track_safe();
    if (!track) {
        return;
    }
    deadbeef->pl_lock();
    const char *uri = deadbeef->pl_find_meta(track, ":URI");
    const char *artist = deadbeef->pl_find_meta(track, "artist");
    const char *title = deadbeef->pl_find_meta(track, "title");
    char *path = uri ? strdup(uri) : NULL;
    char *artist_copy = artist ? strdup(artist) : NULL;
    char *title_copy = title ? strdup(title) : NULL;
    deadbeef->pl_unlock();

    if (path) {
        load_track(panel, path, artist_copy, title_copy);
        ls_artwork_load(&panel->art, track);
    }
    deadbeef->pl_item_unref(track);
    free(path);
    free(artist_copy);
    free(title_copy);
}
