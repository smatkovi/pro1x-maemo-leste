# pro1x-modemd and pro1x-callaudio on the PC (freebsd/modem/README.md, "Tests
# on the PC"): make -C freebsd/modem/test
#   test_sms     the SMS codec against known PDUs and round trips
#   run.sh       pro1x-modemd against a simulated QMI modem (QMUX on a pty)
#                on a private D-Bus, as oFono's clients see it
# All under ASan and UBSan. Needs the host's libqmi-glib and libqrtr-glib,
# python3 with dbus-python and PyGObject, dbus-daemon.
CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-fno-omit-frame-pointer
M = ../pro1x-modemd
QMI = $$(pkg-config --cflags --libs qmi-glib qrtr-glib gio-2.0 gio-unix-2.0)

all: test_sms pro1x-modemd
	./test_sms
	B=$(CURDIR)/pro1x-modemd ./run.sh

test_sms: test_sms.c $(M)/sms.c $(M)/sms.h
	$(CC) $(CFLAGS) -o $@ test_sms.c $(M)/sms.c

pro1x-modemd: $(M)/*.c $(M)/*.h
	$(CC) $(CFLAGS) -o $@ $(M)/*.c $(QMI)

clean:
	rm -f test_sms pro1x-modemd

.PHONY: all clean
