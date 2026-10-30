# pro1x-events: the Events page of Hildon's MeeGo-like look
# (../../tools/pro1x-events) and its control panel applet (the sections,
# the weather's place, the Mastodon account), with their texts.
#
#   make && make install DESTDIR=... PREFIX=/usr          (Leste)
#   make CC=... CXX=... PKG_CONFIG=... PREFIX=/usr/local  (FreeBSD)
#
# The calendar (calendar.cpp, calendar-backend's C++ API) where
# calendar-backend is there, else the page is built without it.
PREFIX ?= /usr
PKG_CONFIG ?= pkg-config
CC ?= cc
CXX ?= c++
MSGFMT ?= msgfmt
LOCALEDIR = $(PREFIX)/share/locale
PAGE ?= ../../tools/pro1x-events/pro1x-events.c
CAL = $(dir $(PAGE))calendar.cpp
PAGE_PKGS = gtk+-2.0 gconf-2.0 gio-2.0 libcurl json-glib-1.0 rtcom-eventlogger
CAL_PKGS = calendar-backend libtime dbus-1 sqlite3 libical glib-2.0
APPLET_PKGS = hildon-1 hildon-control-panel gconf-2.0 libosso gtk+-2.0 libcurl json-glib-1.0
LANGS = de
HAVE_CAL := $(shell $(PKG_CONFIG) --exists calendar-backend && echo 1)

ifeq ($(HAVE_CAL),1)
CAL_OBJ = calendar.o
LINK = $(CXX)
else
CAL_OBJ =
CAL_DEF = -DPRO1X_NO_CALENDAR
LINK = $(CC)
endif

all: pro1x-events libpro1x-events.so $(LANGS:=.mo)

page.o: $(PAGE) $(dir $(PAGE))calendar.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -DLOCALEDIR='"$(LOCALEDIR)"' $(CAL_DEF) \
	    $$($(PKG_CONFIG) --cflags $(PAGE_PKGS)) -c -o $@ $(PAGE)

calendar.o: $(CAL) $(dir $(PAGE))calendar.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall $$($(PKG_CONFIG) --cflags $(CAL_PKGS)) \
	    -c -o $@ $(CAL)

pro1x-events: page.o $(CAL_OBJ)
	$(LINK) $(CFLAGS) -o $@ page.o $(CAL_OBJ) $(LDFLAGS) \
	    $$($(PKG_CONFIG) --libs $(PAGE_PKGS) $(if $(CAL_OBJ),$(CAL_PKGS))) -lX11 -lm

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
	rm -f pro1x-events libpro1x-events.so page.o calendar.o *.mo

.PHONY: all install clean
