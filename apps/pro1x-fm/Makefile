# pro1x-fm: FM radio for Hildon (the WCN3950's receiver, /dev/radio0).
#
#   make && make install DESTDIR=... PREFIX=/usr          (Leste)
PREFIX ?= /usr
PKG_CONFIG ?= pkg-config
CC ?= cc
MSGFMT ?= msgfmt
LOCALEDIR = $(PREFIX)/share/locale
PKGS = hildon-1 gtk+-2.0 gthread-2.0
LANGS = de

all: pro1x-fm $(LANGS:=.mo)

pro1x-fm: pro1x-fm.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -DLOCALEDIR='"$(LOCALEDIR)"' \
	    $$($(PKG_CONFIG) --cflags $(PKGS)) -o $@ pro1x-fm.c \
	    $(LDFLAGS) $$($(PKG_CONFIG) --libs $(PKGS))

%.mo: %.po
	$(MSGFMT) -o $@ $<

install: all
	install -D -m 755 pro1x-fm $(DESTDIR)$(PREFIX)/bin/pro1x-fm
	install -D -m 644 pro1x-fm.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/hildon/pro1x-fm.desktop
	for l in $(LANGS); do \
		install -D -m 644 $$l.mo $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/pro1x-fm.mo; \
	done

clean:
	rm -f pro1x-fm *.mo

.PHONY: all install clean
