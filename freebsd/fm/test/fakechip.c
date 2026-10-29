/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A WCN3950 on a pty, for the host test of pro1x-fmd (run.sh): its
 * Bluetooth bring-up as Linux's hci_qca/btqca drive it, in-band sleep and
 * the FM receiver's commands, with a few made-up stations. It checks what
 * the host does: the power pulses first, the download (the rampatch
 * byte for byte, the NVM with tag 17 patched as patch 0013 does it), no
 * packet to it while the host's side sleeps, and reports at the end.
 *
 * Written from the protocol as qcabt.c/qcafm.c's sources describe it, not
 * from qcabt.c: the parser here is its own.
 *
 *   fakechip <pty-name-file> <rampatch> <nvm> [nostations]
 *
 * The baud rate cannot travel over a pty: the rate code of the baud
 * command is only checked against the NVM's.
 */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum chip { OFF, ROM, PATCHED };

static struct {
	int		fd;
	enum chip	state;
	bool		pulse_off, pulse_on;
	/* in-band sleep, after HCI_RESET */
	bool		ibs;
	bool		host_awake;	/* the host's transmit side */
	bool		chip_awake;	/* ours */
	uint64_t	last_tx, last_rx;
	/* the downloads */
	uint8_t		*ram, *nvm;
	size_t		ram_len, nvm_len;
	uint8_t		got[65536];
	size_t		got_len;
	int		file;		/* 1 rampatch, 2 NVM */
	int		mode;
	uint8_t		baud_code;
	bool		ram_ok, nvm_ok;
	/* the FM receiver */
	bool		fm_on;
	uint32_t	freq;
	bool		nostations;
	uint64_t	rds_at, seek_at;
	int		seek_dir;
	unsigned int	violations, frames, fm_cmds;
	/* the receiver */
	uint8_t		rx[300];
	size_t		rx_len, rx_need;
} c;

static const struct {
	uint32_t	khz;
	int8_t		rssi;
	const char	*ps, *rt;
} stations[] = {
	{ 99900, -70, "TESTFM", "Hello from the fake WCN3950" },
	{ 103800, -78, "OTHER", "Second station" },
};

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void
say(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "fakechip %llu: ", (unsigned long long)now_ms() % 100000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void
out(const uint8_t *p, size_t len)
{
	if (write(c.fd, p, len) != (ssize_t)len)
		say("write: %s", strerror(errno));
	c.last_tx = now_ms();
}

static void
pump_until_ack(void);
static void
run_deferred(void);

/* a packet from us: woken first when our side sleeps */
static void
send_pkt(const uint8_t *p, size_t len)
{
	uint8_t w = 0xfd;

	if (c.ibs && !c.chip_awake) {
		out(&w, 1);
		pump_until_ack();
		c.chip_awake = true;
		out(p, len);
		run_deferred();
		return;
	}
	out(p, len);
}

static void
event(uint8_t evt, const uint8_t *p, uint8_t plen)
{
	uint8_t f[260] = { 0x04, evt, plen };

	memcpy(f + 3, p, plen);
	send_pkt(f, 3 + (size_t)plen);
}

static void
cc(uint16_t op, uint8_t status)
{
	uint8_t p[4] = { 1, op & 0xff, op >> 8, status };

	event(0x0e, p, 4);
}

static void
fm_event(uint8_t evt, const uint8_t *p, uint8_t plen)
{
	uint8_t f[260] = { 0x14, evt, plen };

	memcpy(f + 3, p, plen);
	send_pkt(f, 3 + (size_t)plen);
}

static void
fm_cc(uint16_t op, const uint8_t *rsp, uint8_t len)
{
	uint8_t p[64] = { 1, op & 0xff, op >> 8, 0 };

	if (len > 0)
		memcpy(p + 4, rsp, len);
	fm_event(0x0f, p, 4 + len);
}

static int
station_at(uint32_t khz)
{
	size_t i;

	if (c.nostations)
		return (-1);
	for (i = 0; i < sizeof(stations) / sizeof(stations[0]); i++)
		if (stations[i].khz == khz)
			return ((int)i);
	return (-1);
}

static void
put_station(uint8_t *p)
{
	int s = station_at(c.freq);

	p[0] = c.freq & 0xff;
	p[1] = (c.freq >> 8) & 0xff;
	p[2] = (c.freq >> 16) & 0xff;
	p[3] = c.freq >> 24;
	p[4] = s >= 0;
	p[5] = (uint8_t)(s >= 0 ? stations[s].rssi : -110);
	p[6] = s >= 0;		/* stereo */
	p[7] = s >= 0;		/* RDS sync */
	p[8] = 0;
	p[9] = (uint8_t)(s >= 0 ? 12 : -2);
	p[10] = 0;
}

static void
tune_status(void)
{
	uint8_t p[12] = { 0 };

	put_station(p + 1);
	fm_event(0x01, p, sizeof(p));
	c.rds_at = station_at(c.freq) >= 0 ? now_ms() + 300 : 0;
}

static void
rds(void)
{
	int s = station_at(c.freq);
	uint8_t p[80];
	size_t n;

	if (s < 0)
		return;
	/* PS: PI, PTY, flags, the number of names, then 8 characters */
	memset(p, 0, sizeof(p));
	p[0] = 0xd3; p[1] = 0x01; p[2] = 10; p[7] = 1;
	memset(p + 8, ' ', 8);
	memcpy(p + 8, stations[s].ps, strlen(stations[s].ps));
	fm_event(0x09, p, 16);
	/* RT: PI, PTY, TA/TP, A/B, the text, CR */
	n = strlen(stations[s].rt);
	memset(p, 0, 5);
	p[0] = 0xd3; p[1] = 0x01; p[2] = 10;
	memcpy(p + 5, stations[s].rt, n);
	p[5 + n] = 0x0d;
	fm_event(0x0a, p, (uint8_t)(6 + n));
}

static void
seek_done(void)
{
	uint32_t from = c.freq, f = c.freq;
	uint8_t p[1] = { 0 };

	/* the next station in that direction, around the band */
	do {
		f = c.seek_dir > 0 ? f + 100 : f - 100;
		if (f > 108000)
			f = 87500;
		if (f < 87500)
			f = 108000;
	} while (f != from && station_at(f) < 0);
	c.freq = f;
	fm_event(0x12, p, 1);
	tune_status();
}

static bool
check_nvm(void)
{
	/* type 4 around type 2: tags from offset 8 */
	size_t i = 8;
	uint16_t tag, len;

	if (c.got_len != c.nvm_len)
		return (false);
	while (i + 12 <= c.got_len) {
		tag = (uint16_t)(c.got[i] | c.got[i + 1] << 8);
		len = (uint16_t)(c.got[i + 2] | c.got[i + 3] << 8);
		if (tag == 17) {
			if (c.got[i + 12] != (c.nvm[i + 12] | 0x80) ||
			    c.got[i + 13] != c.baud_code) {
				say("NVM tag 17: %02x %02x, want %02x %02x",
				    c.got[i + 12], c.got[i + 13],
				    c.nvm[i + 12] | 0x80, c.baud_code);
				return (false);
			}
			say("NVM tag 17: %02x %02x %02x %02x %02x %02x "
			    "(patched as 0013 does)", c.got[i + 12],
			    c.got[i + 13], c.got[i + 14], c.got[i + 15],
			    c.got[i + 16], c.got[i + 17]);
		}
		i += 12 + len;
		if (i >= 8 + 1456)
			break;
	}
	return (true);
}

static void
segment(const uint8_t *data, uint8_t len)
{
	uint8_t vse[3] = { 0x00, 0x04, 0x00 };
	bool last;

	if (c.got_len == 0) {
		c.file = data[0] == 1 ? 1 : 2;
		c.mode = c.file == 1 ? data[14] : 0;
	}
	if (c.got_len + len > sizeof(c.got)) {
		say("download too long");
		c.violations++;
		return;
	}
	memcpy(c.got + c.got_len, data, len);
	c.got_len += len;
	last = len < 243 || c.got_len == (c.file == 1 ? c.ram_len : c.nvm_len);
	if (!last && c.mode == 3)
		return;		/* VSE and CC skipped */
	event(0xff, vse, 3);
	if (!last || c.mode != 3)
		cc(0xfc00, 0);
	if (!last)
		return;
	if (c.file == 1) {
		c.ram_ok = c.got_len == c.ram_len &&
		    memcmp(c.got, c.ram, c.ram_len) == 0;
		say("rampatch %zu bytes, mode %d: %s", c.got_len, c.mode,
		    c.ram_ok ? "as the file" : "DIFFERS");
	} else {
		c.nvm_ok = check_nvm();
		say("NVM %zu bytes: %s", c.got_len, c.nvm_ok ? "ok" : "WRONG");
	}
	c.got_len = 0;
}

static void
hci_cmd(uint16_t op, const uint8_t *p, uint8_t plen)
{
	uint8_t ver[14] = { 0x00, 0x02, 0x0f, 0, 0, 0, 0x02, 0x00, 0x02, 0x01,
	    0x20, 0x01, 0x07, 0x40 };

	if (c.state == OFF) {
		say("command %#06x while off", op);
		c.violations++;
		return;
	}
	switch (op) {
	case 0xfc00:
		if (plen >= 1 && p[0] == 0x19) {
			event(0xff, ver, sizeof(ver));
			cc(op, 0);
		} else if (plen >= 2 && p[0] == 0x1e) {
			segment(p + 2, p[1]);
		}
		break;
	case 0xfc48:
		c.baud_code = p[0];
		say("baud rate code %u", p[0]);
		break;
	case 0x0c03:
		c.state = PATCHED;
		cc(op, 0);
		c.ibs = true;
		c.chip_awake = true;
		c.host_awake = false;
		say("reset: %s", c.ram_ok && c.nvm_ok ? "set up" :
		    "WITHOUT the firmware");
		break;
	case 0xfc08:
		/* pre-shutdown: back to off, no in-band sleep */
		cc(op, 0);
		c.state = OFF;
		c.ibs = false;
		break;
	default:
		cc(op, 0x01);	/* unknown command */
		break;
	}
}

static void
fm_cmd(uint16_t op, const uint8_t *p, uint8_t plen)
{
	uint8_t st[12] = { 0 };

	c.fm_cmds++;
	if (c.state != PATCHED) {
		say("FM command %#06x before the setup", op);
		c.violations++;
		return;
	}
	switch (op) {
	case 0x4c01:
		c.fm_on = true;
		fm_cc(op, st, 1);
		break;
	case 0x4c02:
		c.fm_on = false;
		c.rds_at = c.seek_at = 0;
		fm_cc(op, NULL, 0);
		break;
	case 0x5401:
		c.freq = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 |
		    (uint32_t)p[3] << 24);
		fm_cc(op, NULL, 0);
		tune_status();
		break;
	case 0x4c0a:
		put_station(st);
		fm_cc(op, st, 11);
		break;
	case 0x4c0e:
		c.seek_dir = plen >= 3 && p[2] == 0 ? 1 : -1;
		fm_cc(op, NULL, 0);
		c.seek_at = now_ms() + (c.nostations ? 1500 : 400);
		break;
	default:
		if (!c.fm_on && op != 0x4c04) {
			say("FM command %#06x with the receiver off", op);
			c.violations++;
		}
		fm_cc(op, NULL, 0);
		break;
	}
}

static void
frame(const uint8_t *f, size_t len)
{
	(void)len;
	c.frames++;
	if (c.ibs && !c.host_awake) {
		say("frame type %#04x while the host's side sleeps", f[0]);
		c.violations++;
	}
	c.last_rx = now_ms();
	if (f[0] == 0x01)
		hci_cmd((uint16_t)(f[1] | f[2] << 8), f + 4, f[3]);
	else if (f[0] == 0x11)
		fm_cmd((uint16_t)(f[1] | f[2] << 8), f + 4, f[3]);
}

static bool got_ack, waking;
static uint8_t deferred[8][300];
static size_t deferred_len[8];
static int ndeferred;

static void
input(const uint8_t *b, size_t n)
{
	uint8_t ack = 0xfc;
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t x = b[i];

		if (c.rx_len == 0) {
			if (c.state == OFF || (!c.ibs && (x == 0xc0 ||
			    x == 0xfc))) {
				/* the power pulses */
				if (x == 0xc0) {
					c.pulse_off = true;
					c.state = OFF;
					c.ibs = false;
				} else if (x == 0xfc && c.pulse_off) {
					c.pulse_on = true;
					c.state = ROM;
				} else {
					say("byte %02x while off", x);
				}
				continue;
			}
			if (x == 0xfd) {		/* host wakes us */
				c.host_awake = true;
				out(&ack, 1);
				continue;
			}
			if (x == 0xfe) {
				c.host_awake = false;
				continue;
			}
			if (x == 0xfc) {
				got_ack = true;
				continue;
			}
			if (x != 0x01 && x != 0x11) {
				say("unknown byte %02x", x);
				c.violations++;
				continue;
			}
			c.rx[c.rx_len++] = x;
			c.rx_need = 4;
			continue;
		}
		c.rx[c.rx_len++] = x;
		if (c.rx_len == 4)
			c.rx_need = 4 + c.rx[3];
		if (c.rx_len >= c.rx_need) {
			uint8_t f[300];
			size_t flen = c.rx_len;

			/* the receiver free again before answering */
			memcpy(f, c.rx, flen);
			c.rx_len = 0;
			if (waking && ndeferred < 8) {
				/* while we wake the host: afterwards */
				memcpy(deferred[ndeferred], f, flen);
				deferred_len[ndeferred++] = flen;
			} else {
				frame(f, flen);
			}
		}
	}
}

static void
pump_until_ack(void)
{
	struct pollfd pfd = { .fd = c.fd, .events = POLLIN };
	uint8_t b[512];
	uint64_t until = now_ms() + 500;
	ssize_t n;

	got_ack = false;
	waking = true;
	while (!got_ack && now_ms() < until) {
		if (poll(&pfd, 1, 50) <= 0)
			continue;
		if ((n = read(c.fd, b, sizeof(b))) <= 0)
			return;
		input(b, (size_t)n);
	}
	waking = false;
	if (!got_ack) {
		say("no WAKE_ACK from the host");
		c.violations++;
	}
}

/* the frames that came while we woke the host */
static void
run_deferred(void)
{
	int i, n = ndeferred;

	ndeferred = 0;
	for (i = 0; i < n; i++)
		frame(deferred[i], deferred_len[i]);
}

static uint8_t *
load(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p;
	long n;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
		perror(path);
		exit(1);
	}
	rewind(f);
	p = malloc((size_t)n);
	if (p == NULL || fread(p, 1, (size_t)n, f) != (size_t)n)
		exit(1);
	fclose(f);
	*len = (size_t)n;
	return (p);
}

static volatile sig_atomic_t quit;

static void
on_term(int sig)
{
	(void)sig;
	quit = 1;
}

int
main(int argc, char **argv)
{
	struct pollfd pfd;
	struct termios t;
	uint8_t b[512], s = 0xfe;
	FILE *f;
	ssize_t n;
	int timeout;

	if (argc < 4) {
		fprintf(stderr, "usage: fakechip namefile rampatch nvm "
		    "[nostations]\n");
		return (2);
	}
	c.ram = load(argv[2], &c.ram_len);
	c.nvm = load(argv[3], &c.nvm_len);
	c.nostations = argc > 4 && strcmp(argv[4], "nostations") == 0;
	c.freq = 87500;
	if ((c.fd = posix_openpt(O_RDWR | O_NOCTTY)) < 0 ||
	    grantpt(c.fd) != 0 || unlockpt(c.fd) != 0) {
		perror("pty");
		return (1);
	}
	if (tcgetattr(c.fd, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(c.fd, TCSANOW, &t);
	}
	if ((f = fopen(argv[1], "w")) == NULL)
		return (1);
	fprintf(f, "%s\n", ptsname(c.fd));
	fclose(f);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	pfd.fd = c.fd;
	pfd.events = POLLIN;
	while (!quit) {
		timeout = 100;
		if (poll(&pfd, 1, timeout) > 0) {
			n = read(c.fd, b, sizeof(b));
			if (n > 0)
				input(b, (size_t)n);
		}
		if (c.seek_at && now_ms() >= c.seek_at) {
			c.seek_at = 0;
			seek_done();
		}
		if (c.rds_at && now_ms() >= c.rds_at) {
			c.rds_at = 0;
			rds();
		}
		/* our side to sleep after 400 ms without sending */
		if (c.ibs && c.chip_awake && now_ms() - c.last_tx > 400) {
			out(&s, 1);
			c.chip_awake = false;
		}
	}
	say("report: pulses %s, rampatch %s, NVM %s, frames %u, FM commands "
	    "%u, violations %u", c.pulse_off && c.pulse_on ? "ok" : "MISSING",
	    c.ram_ok ? "ok" : "BAD", c.nvm_ok ? "ok" : "BAD", c.frames,
	    c.fm_cmds, c.violations);
	return (c.ram_ok && c.nvm_ok && c.violations == 0 ? 0 : 1);
}
