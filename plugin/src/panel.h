#ifndef LS_PANEL_H
#define LS_PANEL_H

/* gtk.h must come first: gtkui_api.h falls back to `typedef void GtkWidget`
 * when GTK's own headers have not been seen yet, which then collides with
 * the real declaration and buries the build in type errors. */
#include <gtk/gtk.h>

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

typedef struct LsPanel LsPanel;

LsPanel *ls_panel_new(void);
ddb_gtkui_widget_t *ls_panel_as_widget(LsPanel *panel);

/* Load whatever is playing right now, so a panel added mid-song fills in
 * instead of waiting for the next track change. */
void ls_panel_sync_now(LsPanel *panel);

void ls_panel_set_message(LsPanel *panel, const char *message);
void ls_panel_debug_state(LsPanel *panel, const char *when);

#endif /* LS_PANEL_H */
