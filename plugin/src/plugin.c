/* LyricScope — a synced-lyrics panel that docks inside DeaDBeeF.
 *
 * Registration follows deadbeef-lyricbar: a DB_misc_t plugin that, on
 * connect, finds the gtkui plugin and registers a widget type with
 * w_reg_widget. The user then places it from Design Mode like any other
 * built-in panel, which is what makes it genuinely docked — one window,
 * the player's own layout, no separate process to keep alive.
 */

/* Before gtkui_api.h — see the note in panel.h. */
#include <gtk/gtk.h>

#include "artwork.h"
#include "log.h"
#include "panel.h"
#include "pybridge.h"
#include "settings.h"

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

#include <stdlib.h>

DB_functions_t *deadbeef;
/* Not static: panel.c needs w_get_design_mode() to decide whether
 * right-click belongs to Design Mode or to the settings menu. */
ddb_gtkui_t *gtkui_plugin;
static DB_misc_t plugin;

static ddb_gtkui_widget_t *lyricscope_create(void) {
    LsPanel *panel = ls_panel_new();
    if (!panel) {
        return NULL;
    }
    ddb_gtkui_widget_t *widget = ls_panel_as_widget(panel);
    /* Hands the panel the right-click "Design Mode" menu and the rest of
     * the container behaviour every other dockable widget has. */
    gtkui_plugin->w_override_signals(widget->widget, widget);
    /* A panel added part-way through a song should fill in now rather than
     * sit empty until the next track change. */
    ls_panel_sync_now(panel);
    return widget;
}

static int lyricscope_start(void) {
    /* Before anything can draw: every unset key falls back to the
     * compiled-in default, so a first run after this update looks exactly
     * like the build that had no settings at all. */
    ls_settings_load();

    /* Python comes up once, here, rather than on first use: the first
     * import costs ~30ms (mutagen), and paying that during startup is
     * invisible where paying it on the first track change would not be. */
    if (ls_py_init() != 0) {
        LS_LOG("embedded python failed to start; the panel will show errors");
    }
    return 0;
}

static int lyricscope_stop(void) {
    /* Py_Finalize is deliberately not called. Finalizing an interpreter
     * that has imported C extension modules is a long-standing source of
     * shutdown crashes, and there is nothing to gain — the plugin only
     * stops when the process is exiting anyway. */
    gtkui_plugin = NULL;
    return 0;
}

static int lyricscope_connect(void) {
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id(DDB_GTKUI_PLUGIN_ID);
    if (!gtkui_plugin) {
        LS_LOG("can't find the gtkui plugin; nothing to dock into");
        return -1;
    }
    ls_artwork_init();
    gtkui_plugin->w_reg_widget("LyricScope", 0, lyricscope_create, "lyricscope",
                               NULL);
    LS_LOG("registered widget type \"lyricscope\"");
    return 0;
}

static int lyricscope_disconnect(void) {
    if (gtkui_plugin) {
        gtkui_plugin->w_unreg_widget("lyricscope");
    }
    return 0;
}

__attribute__((visibility("default")))
DB_plugin_t *ddb_lyricscope_gtk3_load(DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN(&plugin);
}

static DB_misc_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 5,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = "lyricscope",
    .plugin.name = "LyricScope",
    .plugin.descr =
        "Synced lyrics panel.\n"
        "Reads .lrc sidecars, embedded lyrics tags and a central lyrics\n"
        "folder. Click a line to seek to it.\n",
    .plugin.copyright = "MIT",
    .plugin.start = lyricscope_start,
    .plugin.stop = lyricscope_stop,
    .plugin.connect = lyricscope_connect,
    .plugin.disconnect = lyricscope_disconnect,
};
