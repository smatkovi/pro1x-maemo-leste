# pro1x-events: the Events page of Hildon's MeeGo-like look
# (../../tools/pro1x-events) and its control panel applet (the Mastodon
# account), with their texts.
#
#   make && make install DESTDIR=... PREFIX=/usr          (Leste)
#   make CC=... PKG_CONFIG=... PREFIX=/usr/local          (FreeBSD)
PREFIX ?= /usr
PKG_CONFIG ?= pkg-config
CC ?= cc
MSGFMT ?= msgfmt
LOCALEDIR = $(PREFIX)/share/locale
PAGE ?= ../../tools/pro1x-events/pro1x-events.c
PAGE_PKGS = gtk+-2.0 gconf-2.0 gio-2.0 libcurl json-glib-1.0 rtcom-eventlogger
APPLET_PKGS = hildon-1 hildon-control-panel gconf-2.0 libosso gtk+-2.0
LANGS = de

all: pro1x-events libpro1x-events.so $(LANGS:=.mo)

pro1x-events: $(PAGE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -DLOCALEDIR='"$(LOCALEDIR)"' \
	    $$($(PKG_CONFIG) --cflags $(PAGE_PKGS)) -o $@ $(PAGE) \
	    $(LDFLAGS) $$($(PKG_CONFIG) --libs $(PAGE_PKGS)) -lX11 -lm

libpro1x-events.so: pro1x-events-settings.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -fPIC -shared -DLOCALEDIR='"$(LOCALEDIR)"' \
	    $$($(PKG_CONFIG) --cflags $(APPLET_PKGS)) -o $@ pro1x-events-settings.c \
	    $(LDFLAGS) $$($(PKG_CONFIG) --libs $(APPLET_PKGS))

%.mo: %.po
	$(MSGFMT) -o $@ $<

install: all
	install -D -m 755 pro1x-events $(DESTDIR)$(PREFIX)/bin/pro1x-events
	install -D -m 644 libpro1x-events.so \
	    $(DESTDIR)$(PREFIX)/lib/hildon-control-panel/libpro1x-events.so
	install -D -m 644 pro1x-events.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/hildon-control-panel/pro1x-events.desktop
	for l in $(LANGS); do \
		install -D -m 644 $$l.mo $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/pro1x-events.mo; \
	done

clean:
	rm -f pro1x-events libpro1x-events.so *.mo

.PHONY: all install clean
