# pro1x-modemd and pro1x-callaudio on the PC (freebsd/modem/README.md, "Tests
# on the PC"): make -C freebsd/modem/test
#   test_sms     the SMS codec against known PDUs and round trips
#   test_callaudio.sh   pro1x-callaudio against a stand-in for pro1x-audiod
#   run.sh       pro1x-modemd against a simulated QMI modem (QMUX on a pty)
#                on a private D-Bus, as oFono's clients see it, with
#                pro1x-callaudio per call
# All under ASan and UBSan. Needs the host's libqmi-glib and libqrtr-glib,
# python3 with dbus-python and PyGObject, dbus-daemon.
CFLAGS = -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
	-fno-omit-frame-pointer
M = ../pro1x-modemd
A = ../pro1x-callaudio
QMI = $$(pkg-config --cflags --libs qmi-glib qrtr-glib gio-2.0 gio-unix-2.0)

all: test_sms pro1x-modemd pro1x-callaudio
	./test_sms
	A=$(CURDIR)/pro1x-callaudio ./test_callaudio.sh
	B=$(CURDIR)/pro1x-modemd CA=$(CURDIR)/pro1x-callaudio ./run.sh

test_sms: test_sms.c $(M)/sms.c $(M)/sms.h
	$(CC) $(CFLAGS) -o $@ test_sms.c $(M)/sms.c

pro1x-modemd: $(M)/*.c $(M)/*.h
	$(CC) $(CFLAGS) -o $@ $(M)/*.c $(QMI)

pro1x-callaudio: $(A)/*.c
	$(CC) $(CFLAGS) -o $@ $(A)/*.c

clean:
	rm -f test_sms pro1x-modemd pro1x-callaudio

.PHONY: all clean
