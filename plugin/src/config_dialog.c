/* The settings dialog.
 *
 * Built in code rather than from a Glade resource. Lyricbar uses
 * GtkBuilder against a compiled-in panels.glade, which is the right call
 * for its dialog — three colour buttons, two spinners, two radio groups
 * and a save-method section — but it drags in glib-compile-resources, a
 * generated resources.c, and a 53KB .glade file to maintain alongside the
 * code. Five controls do not earn that, and the ask here was explicitly
 * "functional native widgets, no styling": a GtkGrid of stock widgets
 * picks up Mint's theme on its own and needs no CSS.
 *
 * Every control applies live. The values are only written to the config on
 * OK; Cancel puts back what was there when the dialog opened, so the panel
 * is never left showing something that was never saved.
 */

#include "config_dialog.h"

#include "animations.h"
#include "log.h"
#include "settings.h"
#include "theme.h"

#include <string.h>

typedef struct {
    GtkWidget *dialog;
    LsSettings original; /* restored on Cancel */
    void (*changed)(void *);
    void (*blur_changed)(void *);
    void *user_data;

    GtkWidget *text_px;
    GtkWidget *accent;
    GtkWidget *blur;
    GtkWidget *transition;
    GtkWidget *alignment;
} LsConfigDialog;

/* One at a time: the settings are global, so a second dialog would just be
 * two views fighting over the same values. */
static LsConfigDialog *open_dialog;

static void notify(LsConfigDialog *self) {
    if (self->changed) {
        self->changed(self->user_data);
    }
}

static void notify_blur(LsConfigDialog *self) {
    if (self->blur_changed) {
        self->blur_changed(self->user_data);
    }
}

/* -- control handlers -------------------------------------------------- */

static void on_text_px(GtkSpinButton *spin, gpointer data) {
    LsConfigDialog *self = data;
    ls_settings.text_px = gtk_spin_button_get_value_as_int(spin);
    notify(self);
}

static void on_accent(GtkColorButton *button, gpointer data) {
    LsConfigDialog *self = data;
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    /* Stored as #rrggbb, the same shape lyricbar uses for its colours and
     * the only one gdk_rgba_parse is guaranteed to round-trip. */
    snprintf(ls_settings.accent, sizeof(ls_settings.accent), "#%02x%02x%02x",
             (int)(rgba.red * 255.0 + 0.5), (int)(rgba.green * 255.0 + 0.5),
             (int)(rgba.blue * 255.0 + 0.5));
    notify(self);
}

static void on_blur(GtkSpinButton *spin, gpointer data) {
    LsConfigDialog *self = data;
    ls_settings.blur_radius = gtk_spin_button_get_value_as_int(spin);
    notify_blur(self);
}

static void on_transition(GtkSpinButton *spin, gpointer data) {
    LsConfigDialog *self = data;
    ls_settings.transition_ms = gtk_spin_button_get_value_as_int(spin);
    notify(self);
}

static void on_alignment(GtkComboBox *combo, gpointer data) {
    LsConfigDialog *self = data;
    const int active = gtk_combo_box_get_active(combo);
    if (active >= 0) {
        ls_settings.alignment = active;
        notify(self);
    }
}

/* -- dialog ------------------------------------------------------------ */

static void on_response(GtkDialog *dialog, gint response, gpointer data) {
    LsConfigDialog *self = data;

    if (response == GTK_RESPONSE_OK) {
        ls_settings_save();
    } else {
        const int blur_differed =
            self->original.blur_radius != ls_settings.blur_radius;
        ls_settings = self->original;
        if (blur_differed) {
            notify_blur(self);
        } else {
            notify(self);
        }
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
    open_dialog = NULL;
    g_free(self);
}

static GtkWidget *add_row(GtkWidget *grid, int row, const char *label,
                          GtkWidget *control) {
    GtkWidget *text = gtk_label_new(label);
    gtk_widget_set_halign(text, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    return control;
}

void ls_config_dialog_show(GtkWidget *parent,
                           void (*changed)(void *user_data),
                           void (*blur_changed)(void *user_data),
                           void *user_data) {
    if (open_dialog) {
        gtk_window_present(GTK_WINDOW(open_dialog->dialog));
        return;
    }

    LsConfigDialog *self = g_malloc0(sizeof(LsConfigDialog));
    self->original = ls_settings;
    self->changed = changed;
    self->blur_changed = blur_changed;
    self->user_data = user_data;

    GtkWidget *toplevel = gtk_widget_get_toplevel(parent);
    self->dialog = gtk_dialog_new_with_buttons(
        "LyricScope Settings",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel", GTK_RESPONSE_CANCEL, "_OK",
        GTK_RESPONSE_OK, NULL);
    gtk_window_set_resizable(GTK_WINDOW(self->dialog), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(self->dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(content), grid);

    /* Pixels, not points, and labelled as such. The line size is set with
     * pango_font_description_set_absolute_size precisely so it lands on a
     * device pixel without the font engine rounding it; offering it in
     * points would put that rounding back between the user's number and
     * what they see. */
    self->text_px = add_row(
        grid, 0, "Text size (px):",
        gtk_spin_button_new_with_range(LS_TEXT_PX_MIN, LS_TEXT_PX_MAX, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->text_px),
                              ls_settings.text_px);

    GdkRGBA accent;
    if (!gdk_rgba_parse(&accent, ls_settings.accent)) {
        gdk_rgba_parse(&accent, LS_ACCENT_DEFAULT);
    }
    self->accent = add_row(grid, 1, "Current line colour:",
                           gtk_color_button_new_with_rgba(&accent));
    gtk_widget_set_halign(self->accent, GTK_ALIGN_START);

    self->blur = add_row(
        grid, 2, "Backdrop blur:",
        gtk_spin_button_new_with_range(0, LS_BLUR_RADIUS_MAX, 2));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->blur),
                              ls_settings.blur_radius);
    gtk_widget_set_tooltip_text(self->blur, "0 leaves the cover art sharp");

    self->transition = add_row(grid, 3, "Line animation (ms):",
                               gtk_spin_button_new_with_range(
                                   LS_TRANSITION_MS_MIN, LS_TRANSITION_MS_MAX, 10));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->transition),
                              ls_settings.transition_ms);
    gtk_widget_set_tooltip_text(self->transition,
                                "Expand/shrink duration; 0 snaps instantly");

    self->alignment = add_row(grid, 4, "Text alignment:", gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self->alignment), "Left");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self->alignment), "Centre");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(self->alignment), "Right");
    gtk_combo_box_set_active(GTK_COMBO_BOX(self->alignment),
                             ls_settings.alignment);

    g_signal_connect(self->text_px, "value-changed", G_CALLBACK(on_text_px), self);
    g_signal_connect(self->accent, "color-set", G_CALLBACK(on_accent), self);
    g_signal_connect(self->blur, "value-changed", G_CALLBACK(on_blur), self);
    g_signal_connect(self->transition, "value-changed",
                     G_CALLBACK(on_transition), self);
    g_signal_connect(self->alignment, "changed", G_CALLBACK(on_alignment), self);
    g_signal_connect(self->dialog, "response", G_CALLBACK(on_response), self);

    open_dialog = self;
    gtk_widget_show_all(self->dialog);
}
