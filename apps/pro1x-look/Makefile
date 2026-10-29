# pro1x-look: the control panel applet for Hildon's desktop style.
#
#   make && make install DESTDIR=... PREFIX=/usr          (Leste)
#   make CC=... PKG_CONFIG=... PREFIX=/usr/local          (FreeBSD)
PREFIX ?= /usr
PKG_CONFIG ?= pkg-config
CC ?= cc
MSGFMT ?= msgfmt
LOCALEDIR = $(PREFIX)/share/locale
PKGS = hildon-1 hildon-control-panel gconf-2.0 libosso gtk+-2.0
LANGS = de

all: libpro1x-look.so $(LANGS:=.mo)

libpro1x-look.so: pro1x-look.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -fPIC -shared -DLOCALEDIR='"$(LOCALEDIR)"' \
	    $$($(PKG_CONFIG) --cflags $(PKGS)) -o $@ pro1x-look.c \
	    $(LDFLAGS) $$($(PKG_CONFIG) --libs $(PKGS))

%.mo: %.po
	$(MSGFMT) -o $@ $<

install: all
	install -D -m 644 libpro1x-look.so $(DESTDIR)$(PREFIX)/lib/hildon-control-panel/libpro1x-look.so
	install -D -m 644 pro1x-look.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/hildon-control-panel/pro1x-look.desktop
	for l in $(LANGS); do \
		install -D -m 644 $$l.mo $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/pro1x-look.mo; \
	done

clean:
	rm -f libpro1x-look.so *.mo

.PHONY: all install clean
