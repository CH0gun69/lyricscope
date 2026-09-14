#ifndef LS_SPECTRUM_H
#define LS_SPECTRUM_H

/* gtk.h first — see the note in panel.h. */
#include <gtk/gtk.h>

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

typedef struct LsSpectrum LsSpectrum;

LsSpectrum *ls_spectrum_new(void);
ddb_gtkui_widget_t *ls_spectrum_as_widget(LsSpectrum *spectrum);

#endif /* LS_SPECTRUM_H */
