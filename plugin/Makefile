# LyricScope — DeaDBeeF GTK3 plugin.
#
#   make            build ddb_lyricscope_gtk3.so
#   make install    copy it into ~/.local/lib/deadbeef
#   make clean

PLUGIN  = ddb_lyricscope_gtk3.so

# Where the .so goes, and where the Python backend goes. The plugin finds
# the latter at runtime via $XDG_DATA_HOME (or ~/.local/share) — it is NOT
# compiled in, so the repo can live anywhere and can be moved afterwards.
PREFIX   ?= $(HOME)/.local/lib/deadbeef
DATADIR  ?= $(if $(XDG_DATA_HOME),$(XDG_DATA_HOME),$(HOME)/.local/share)/lyricscope
BACKEND   = ../linux/core

PKGS    = gtk+-3.0 gdk-pixbuf-2.0 cairo pango pangocairo
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   = $(shell pkg-config --libs $(PKGS))

# --embed is what adds -lpython; without it the link fails on Py_* symbols
# even though the include path is right.
PY_CFLAGS  = $(shell python3-config --includes)
PY_LIBS    = $(shell python3-config --ldflags --embed)

CFLAGS  += -std=c11 -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE
CFLAGS  += $(PKG_CFLAGS) $(PY_CFLAGS)
LDFLAGS += -shared
LIBS     = $(PKG_LIBS) $(PY_LIBS) -lm

OBJS = src/plugin.o src/panel.o src/artwork.o src/pybridge.o \
       src/settings.o src/config_dialog.o

all: $(PLUGIN)

$(PLUGIN): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(PLUGIN)
	install -d $(PREFIX)
	install -m 0755 $(PLUGIN) $(PREFIX)/
	install -d $(DATADIR)/core
	install -m 0644 $(BACKEND)/*.py $(DATADIR)/core/
	@echo "installed $(PREFIX)/$(PLUGIN)"
	@echo "installed python backend to $(DATADIR)/core"
	@echo "restart DeaDBeeF to load it"

uninstall:
	rm -f $(PREFIX)/$(PLUGIN)
	rm -rf $(DATADIR)

clean:
	rm -f $(OBJS) $(PLUGIN)

.PHONY: all install uninstall clean
