#ifndef LS_CONFIG_DIALOG_H
#define LS_CONFIG_DIALOG_H

#include <gtk/gtk.h>

/* Open the settings dialog, or present the one already open.
 *
 * `changed` is called on the GTK main thread every time a control moves,
 * so the panel can re-render live, and again if the dialog is cancelled
 * and the previous values are put back. `blur_changed` is separate because
 * a blur change is the only one that has to throw away and rebuild a
 * cached surface rather than just repaint.
 */
void ls_config_dialog_show(GtkWidget *parent,
                           void (*changed)(void *user_data),
                           void (*blur_changed)(void *user_data),
                           void *user_data);

#endif /* LS_CONFIG_DIALOG_H */
