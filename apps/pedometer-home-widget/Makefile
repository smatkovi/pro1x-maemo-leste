# Pedometer Home Widget for Maemo Leste (F(x)tec Pro1X)
PKGS = hildon-1 libhildondesktop-1 gio-2.0 gtk+-2.0
CFLAGS ?= -O2 -Wall
CFLAGS += -fPIC $(shell pkg-config --cflags $(PKGS))
LIBS = $(shell pkg-config --libs $(PKGS)) -lm
LIBDIR = $(shell pkg-config libhildondesktop-1 --variable=hildondesktoplibdir)
DESKTOPDIR = $(shell pkg-config libhildondesktop-1 --variable=hildonhomedesktopentrydir)

all: pedometer-home-widget.so

pedometer-home-widget.so: src/pedometer-home-widget.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LIBS)

install: all
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DESKTOPDIR)
	install -m 644 pedometer-home-widget.so $(DESTDIR)$(LIBDIR)
	install -m 644 data/pedometer-home-widget.desktop $(DESTDIR)$(DESKTOPDIR)

clean:
	rm -f pedometer-home-widget.so

.PHONY: all install clean
