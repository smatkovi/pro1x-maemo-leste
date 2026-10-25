# headphoned for Maemo Leste
CFLAGS ?= -O2 -Wall
CFLAGS += $(shell pkg-config --cflags gio-unix-2.0)
LIBS = $(shell pkg-config --libs gio-unix-2.0)
PREFIX ?= /usr/local

all: headphoned

headphoned: headphoned.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: all
	install -D -m 755 headphoned $(DESTDIR)$(PREFIX)/bin/headphoned
	install -D -m 755 45headphoned $(DESTDIR)/etc/X11/Xsession.post/45headphoned
	install -D -m 644 85-headphoned-jack.rules $(DESTDIR)/etc/udev/rules.d/85-headphoned-jack.rules

clean:
	rm -f headphoned

.PHONY: all install clean
