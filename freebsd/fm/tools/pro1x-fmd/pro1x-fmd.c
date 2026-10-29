/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-fmd -- the FM radio of the F(x)tec Pro1X's WCN3950 on FreeBSD.
 *
 * FreeBSD has no Bluetooth stack for the WCN3950 and no V4L2: this daemon
 * owns the Bluetooth UART (the GENI UART at 4a8c000, qcom_geni_uart.ko,
 * /dev/cuau0), powers the chip (its four rails at the RPM, through
 * qcom_dsp's hw.qcom_rpm.regulator), brings it up as Linux's hci_qca and
 * btqca do (qcabt.c), drives the FM receiver as Linux's radio-qca-fm does
 * (qcafm.c), and serves it to the app and the command line on a Unix
 * socket, one text line per request and per answer:
 *
 *   on | off                    the receiver (and the chip) on or off
 *   tune <kHz | MHz.d>          "ok <kHz>"
 *   seek up | down              "ok <kHz>", or "error ENODATA ..."
 *   mute 0|1, mono 0|1
 *   status                      "ok on=1 freq=99900 rssi=-85 signal=87 ..."
 *   station                     the same, asked of the chip first
 *   ps, rt                      "ok <the station name / radio text>"
 *   info                        the chip, the firmware, the counters
 *   debug 0|1|2                 frames (2: and in-band sleep) to the log
 *   watch                       from now on "event <what>" lines too
 *
 * The audio does not come this way: the chip sends it over SLIMbus to the
 * ADSP (freebsd/fm/README.md), which FreeBSD does not drive yet.
 *
 *   pro1x-fmd [-F] [-d tty] [-s speed] [-f fwdir:...] [-r l9,l16,...|none]
 *             [-S socket] [-v]
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/sysctl.h>
#include <dev/ofw/openfirmio.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "qcabt.h"
#include "qcafm.h"

#define	DEF_TTY		"/dev/cuau0"
#define	DEF_SOCKET	"/var/run/pro1x-fm.sock"
#define	DEF_FWDIRS	"/dev/pro1x-host/freebsd/firmware:" \
			"/usr/local/share/pro1x/firmware:/boot/firmware"
#define	MAX_CLIENTS	8
#define	LINE_MAX_	256

static struct {
	int		fd;		/* the UART */
	const char	*tty;
	const char	*fwdirs;
	const char	*socket;
	const char	*rails;		/* NULL: from the device tree */
	bool		voted;
	char		voted_rails[64];
	unsigned int	speed;
	uint64_t	t0;
	bool		foreground;
	volatile sig_atomic_t quit;
} d = {
	.fd = -1, .tty = DEF_TTY, .fwdirs = DEF_FWDIRS, .socket = DEF_SOCKET,
	.speed = 3000000,
};

static struct qbt bt;
static struct qfm fm;

struct client {
	int	fd;
	bool	watch;
	char	buf[LINE_MAX_];
	size_t	len;
};
static struct client clients[MAX_CLIENTS];

/* --- the OS hooks of qcabt ------------------------------------------------ */

static uint64_t
mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void
logmsg(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	uint64_t t = mono_ms() - d.t0;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "pro1x-fmd %3u.%03u %s\n", (unsigned)(t / 1000),
	    (unsigned)(t % 1000), buf);
}

static int
o_write(void *arg, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	struct pollfd pfd = { .fd = d.fd, .events = POLLOUT };
	ssize_t n;

	(void)arg;
	while (len > 0) {
		n = write(d.fd, p, len);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			return (-errno);
		/* flow control holds it: up to 2 s */
		if (poll(&pfd, 1, 2000) == 0)
			return (-ETIMEDOUT);
	}
	return (0);
}

static int
o_read(void *arg, void *buf, size_t len, int timeout_ms)
{
	struct pollfd pfd = { .fd = d.fd, .events = POLLIN };
	ssize_t n;
	int r;

	(void)arg;
	r = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
	if (r < 0)
		return (errno == EINTR ? 0 : -errno);
	if (r == 0)
		return (0);
	n = read(d.fd, buf, len);
	if (n < 0)
		return (errno == EAGAIN || errno == EINTR ? 0 : -errno);
	if (n == 0)
		return (-EIO);		/* the other side of a pty gone */
	return ((int)n);
}

#ifdef __linux__
static speed_t
speed_code(unsigned int baud)
{
	switch (baud) {
	case 2400:	return (B2400);
	case 115200:	return (B115200);
	case 921600:	return (B921600);
	case 3000000:	return (B3000000);
	default:	return (B0);
	}
}
#else
#define	speed_code(b)	((speed_t)(b))
#endif

static int
o_set_speed(void *arg, unsigned int baud)
{
	struct termios t;

	(void)arg;
	if (tcgetattr(d.fd, &t) != 0)
		return (-errno);
	cfsetispeed(&t, speed_code(baud));
	cfsetospeed(&t, speed_code(baud));
	if (tcsetattr(d.fd, TCSANOW, &t) != 0)
		return (-errno);
	return (0);
}

static int
o_set_flow(void *arg, bool on)
{
	struct termios t;
	int rts = TIOCM_RTS;

	(void)arg;
	if (tcgetattr(d.fd, &t) != 0)
		return (-errno);
	if (on)
		t.c_cflag |= CRTSCTS;
	else
		t.c_cflag &= ~CRTSCTS;
	if (tcsetattr(d.fd, TCSANOW, &t) != 0)
		return (-errno);
	/* RTS with it (a pty has none: ignored) */
	(void)ioctl(d.fd, on ? TIOCMBIS : TIOCMBIC, &rts);
	return (0);
}

static int
o_drain(void *arg)
{
	(void)arg;
	return (tcdrain(d.fd) == 0 ? 0 : -errno);
}

static void
o_flush_input(void *arg)
{
	(void)arg;
	tcflush(d.fd, TCIFLUSH);
}

static uint64_t
o_now_ms(void *arg)
{
	(void)arg;
	return (mono_ms());
}

static void
o_sleep_ms(void *arg, unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	(void)arg;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static void
o_log(void *arg, const char *msg)
{
	(void)arg;
	logmsg("%s", msg);
}

/* name is "qca/<file>": the first directory of the list that has it */
static int
o_fw_load(void *arg, const char *name, uint8_t **data, size_t *len)
{
	char path[512], dirs[512], *dir, *save;
	struct stat st;
	uint8_t *buf;
	ssize_t n;
	int fd;

	(void)arg;
	snprintf(dirs, sizeof(dirs), "%s", d.fwdirs);
	for (dir = strtok_r(dirs, ":", &save); dir != NULL;
	    dir = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
		    st.st_size > (1 << 20) ||
		    (buf = malloc((size_t)st.st_size)) == NULL) {
			close(fd);
			return (-EIO);
		}
		n = read(fd, buf, (size_t)st.st_size);
		close(fd);
		if (n != st.st_size) {
			free(buf);
			return (-EIO);
		}
		logmsg("firmware %s", path);
		*data = buf;
		*len = (size_t)n;
		return (0);
	}
	return (-ENOENT);
}

static const struct qbt_ops ops = {
	.write = o_write,
	.read = o_read,
	.set_speed = o_set_speed,
	.set_flow = o_set_flow,
	.drain = o_drain,
	.flush_input = o_flush_input,
	.now_ms = o_now_ms,
	.sleep_ms = o_sleep_ms,
	.log = o_log,
	.fw_load = o_fw_load,
};

/* --- the chip's rails ---------------------------------------------------- */

#ifdef __FreeBSD__
/*
 * The Bluetooth node's supplies (vddio, vddxo, vddrf, vddch0, in the order
 * of Linux's qca_soc_data_wcn3950) by the names of their regulator nodes,
 * from the device tree through /dev/openfirm.
 */
static int
of_prop(int fd, uint32_t node, const char *name, void *buf, int len)
{
	struct ofiocdesc od = { .of_nodeid = node,
	    .of_namelen = (int)strlen(name), .of_name = name,
	    .of_buflen = len, .of_buf = buf };

	if (ioctl(fd, OFIOCGET, &od) != 0)
		return (-1);
	return (od.of_buflen);
}

static uint32_t
of_walk(int fd, uint32_t node, const char *prop, const void *val, int vlen)
{
	char buf[256];
	uint32_t child, found;
	int n;

	for (child = node; child != 0;) {
		n = of_prop(fd, child, prop, buf, sizeof(buf));
		if (n >= vlen && memcmp(buf, val, (size_t)vlen) == 0)
			return (child);
		found = child;
		if (ioctl(fd, OFIOCGETCHILD, &found) == 0 && found != 0 &&
		    (found = of_walk(fd, found, prop, val, vlen)) != 0)
			return (found);
		if (ioctl(fd, OFIOCGETNEXT, &child) != 0)
			break;
	}
	return (0);
}

static int
rails_from_dt(char *out, size_t size)
{
	static const char *supplies[] = { "vddio-supply", "vddxo-supply",
	    "vddrf-supply", "vddch0-supply" };
	static const char compat[] = "qcom,wcn3950-bt";
	uint32_t root = 0, bt_node, reg, xref;
	char name[32];
	size_t i;
	int fd, n;

	out[0] = '\0';
	if ((fd = open("/dev/openfirm", O_RDONLY)) < 0)
		return (-errno);
	if (ioctl(fd, OFIOCGETNEXT, &root) != 0 || root == 0 ||
	    (bt_node = of_walk(fd, root, "compatible", compat,
	    sizeof(compat))) == 0) {
		close(fd);
		return (-ENOENT);
	}
	for (i = 0; i < nitems(supplies); i++) {
		if (of_prop(fd, bt_node, supplies[i], &xref,
		    sizeof(xref)) != 4)
			continue;
		/* the phandle is big endian, as the property is */
		if ((reg = of_walk(fd, root, "phandle", &xref, 4)) == 0)
			continue;
		n = of_prop(fd, reg, "name", name, sizeof(name) - 1);
		if (n <= 0)
			continue;
		name[n] = '\0';
		snprintf(out + strlen(out), size - strlen(out), "%s%s",
		    out[0] ? "," : "", name);
	}
	close(fd);
	return (out[0] ? 0 : -ENOENT);
}

static int
vote(const char *rail, bool on)
{
	char req[32];

	snprintf(req, sizeof(req), "%s %d", rail, on);
	if (sysctlbyname("hw.qcom_rpm.regulator", NULL, NULL, req,
	    strlen(req)) != 0)
		return (-errno);
	return (0);
}
#else
static int
rails_from_dt(char *out, size_t size)
{
	(void)out, (void)size;
	return (-ENOTSUP);
}

static int
vote(const char *rail, bool on)
{
	(void)rail, (void)on;
	return (-ENOTSUP);
}
#endif

static int
rails(bool on)
{
	char list[64], *r, *save;
	int error = 0, e;

	if (on == d.voted)
		return (0);
	if (on) {
		if (d.rails != NULL)
			snprintf(d.voted_rails, sizeof(d.voted_rails), "%s",
			    d.rails);
		else if ((e = rails_from_dt(d.voted_rails,
		    sizeof(d.voted_rails))) != 0) {
			/* sm6115-fxtec-pro1x.dts: L9, L16, L17, L23 */
			logmsg("supplies not found in the device tree (%d), "
			    "taking l9,l16,l17,l23", e);
			snprintf(d.voted_rails, sizeof(d.voted_rails),
			    "l9,l16,l17,l23");
		}
		if (strcmp(d.voted_rails, "none") == 0) {
			d.voted = true;
			return (0);
		}
	} else if (strcmp(d.voted_rails, "none") == 0) {
		d.voted = false;
		return (0);
	}
	snprintf(list, sizeof(list), "%s", d.voted_rails);
	for (r = strtok_r(list, ",", &save); r != NULL;
	    r = strtok_r(NULL, ",", &save)) {
		e = vote(r, on);
		logmsg("rail %s %s: %s", r, on ? "on" : "off",
		    e == 0 ? "voted" : strerror(-e));
		if (e != 0 && error == 0)
			error = e;
	}
	d.voted = on;
	return (on ? error : 0);
}

/* --- the radio -------------------------------------------------------------- */

static const char *
errname(int error)
{
	switch (-error) {
	case QFM_ENOSTATION: return ("ENODATA");
	case ETIMEDOUT:	return ("ETIMEDOUT");
	case ENETDOWN:	return ("ENETDOWN");
	case ENODEV:	return ("ENODEV");
	case ENOENT:	return ("ENOENT");
	case EPROTO:	return ("EPROTO");
	case EINVAL:	return ("EINVAL");
	case EBUSY:	return ("EBUSY");
	default:	return ("EIO");
	}
}

static int
tty_open(void)
{
	struct termios t;

	if (d.fd >= 0)
		return (0);
	d.fd = open(d.tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (d.fd < 0) {
		logmsg("%s: %s", d.tty, strerror(errno));
		return (-ENODEV);
	}
	if (tcgetattr(d.fd, &t) == 0) {
		cfmakeraw(&t);
		t.c_cflag |= CLOCAL | CREAD | CS8 | CRTSCTS;
		t.c_cflag &= ~(CSTOPB | PARENB);
		cfsetispeed(&t, speed_code(115200));
		cfsetospeed(&t, speed_code(115200));
		tcsetattr(d.fd, TCSANOW, &t);
	}
	return (0);
}

static void
tty_close(void)
{
	if (d.fd >= 0)
		close(d.fd);
	d.fd = -1;
}

static int
radio_on(void)
{
	int error;

	if (fm.on)
		return (0);
	if (!bt.up) {
		if ((error = tty_open()) != 0)
			return (error);
		if ((error = rails(true)) != 0)
			logmsg("not every rail voted (%s): trying anyway",
			    strerror(-error));
		if ((error = qbt_power_on(&bt, 3)) != 0) {
			logmsg("the controller did not come up: %s",
			    strerror(-error));
			rails(false);
			tty_close();
			return (-ENODEV);
		}
	}
	if ((error = qfm_on(&fm)) != 0)
		logmsg("the receiver did not come on: %s", strerror(-error));
	else
		logmsg("receiver on at %u kHz%s", fm.freq,
		    fm.slimbus ? ", SLIMbus port on" : "");
	return (error);
}

static void
radio_off(void)
{
	qfm_off(&fm);
	if (bt.up)
		qbt_power_off(&bt);
	rails(false);
	tty_close();
}

static void
status_line(char *out, size_t size)
{
	snprintf(out, size, "ok on=%d freq=%u rssi=%d signal=%d stereo=%u "
	    "rds=%u mute=%d mono=%d pty=%u sinr=%d slimbus=%d", fm.on,
	    fm.freq, fm.st.rssi, fm.on ? qfm_signal(fm.st.rssi) : 0,
	    fm.st.stereo, fm.st.rds_sync, fm.mute, fm.mono, fm.pty,
	    fm.st.sinr, fm.slimbus);
}

static void
broadcast(const char *line)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].watch) {
			if (write(clients[i].fd, line, strlen(line)) < 0) {
				/* the next poll notices it gone */
			}
		}
}

static void
fm_changed(void *arg, const char *what)
{
	char line[LINE_MAX_];

	(void)arg;
	if (strcmp(what, "ps") == 0)
		snprintf(line, sizeof(line), "event ps %s\n", fm.ps);
	else if (strcmp(what, "rt") == 0)
		snprintf(line, sizeof(line), "event rt %s\n", fm.rt);
	else if (strcmp(what, "tune") == 0)
		snprintf(line, sizeof(line), "event tune %u rssi=%d stereo=%u\n",
		    fm.freq, fm.st.rssi, fm.st.stereo);
	else
		snprintf(line, sizeof(line), "event %s\n", what);
	broadcast(line);
}

static uint32_t
parse_freq(const char *s)
{
	char *end;
	double mhz;
	unsigned long khz;

	if (strchr(s, '.') != NULL) {
		mhz = strtod(s, &end);
		return (*end ? 0 : (uint32_t)(mhz * 1000 + 0.5));
	}
	khz = strtoul(s, &end, 10);
	return (*end ? 0 : (uint32_t)khz);
}

/* one request line: the answer into out (newline added by the caller) */
static void
request(struct client *c, char *line, char *out, size_t size)
{
	char *cmd, *arg, *save;
	struct fm_station st;
	uint32_t khz;
	int error = 0;

	cmd = strtok_r(line, " \t", &save);
	arg = strtok_r(NULL, " \t", &save);
	if (cmd == NULL) {
		snprintf(out, size, "error EINVAL empty request");
		return;
	}
	if (strcmp(cmd, "on") == 0) {
		error = radio_on();
	} else if (strcmp(cmd, "off") == 0) {
		radio_off();
	} else if (strcmp(cmd, "tune") == 0) {
		if (arg == NULL || (khz = parse_freq(arg)) == 0)
			error = -EINVAL;
		else if ((error = qfm_tune(&fm, khz)) == 0) {
			snprintf(out, size, "ok %u", fm.freq);
			return;
		}
	} else if (strcmp(cmd, "seek") == 0) {
		if (arg == NULL || (strcmp(arg, "up") && strcmp(arg, "down")))
			error = -EINVAL;
		else if ((error = qfm_seek(&fm, strcmp(arg, "up") == 0)) == 0) {
			snprintf(out, size, "ok %u", fm.freq);
			return;
		}
	} else if (strcmp(cmd, "mute") == 0 || strcmp(cmd, "mono") == 0) {
		if (arg == NULL || (strcmp(arg, "0") && strcmp(arg, "1")))
			error = -EINVAL;
		else if (cmd[1] == 'u')
			error = qfm_set_mute(&fm, arg[0] == '1');
		else
			error = qfm_set_mono(&fm, arg[0] == '1');
	} else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "station") == 0) {
		if (strcmp(cmd, "station") == 0 && fm.on &&
		    (error = qfm_station(&fm, &st)) != 0)
			goto fail;
		status_line(out, size);
		return;
	} else if (strcmp(cmd, "ps") == 0) {
		snprintf(out, size, "ok %s", fm.ps);
		return;
	} else if (strcmp(cmd, "rt") == 0) {
		snprintf(out, size, "ok %s", fm.rt);
		return;
	} else if (strcmp(cmd, "info") == 0) {
		snprintf(out, size, "ok up=%d soc=0x%08x rom=0x%04x patch=0x%04x "
		    "product=0x%08x files=%s,%s speed=%u ibs_wakes=%u/%u "
		    "ibs_sleeps=%u/%u ibs_wacks=%u/%u rx_errors=%u "
		    "cmd_timeouts=%u rails=%s ps=%u rt=%u", bt.up,
		    bt.ver.soc_id, bt.ver.rom_ver, bt.ver.patch_ver,
		    bt.ver.product_id, bt.fw_rampatch, bt.fw_nvm, d.speed,
		    bt.ibs_sent_wakes, bt.ibs_recv_wakes, bt.ibs_sent_slps,
		    bt.ibs_recv_slps, bt.ibs_sent_wacks, bt.ibs_recv_wacks,
		    bt.rx_errors, bt.cmd_timeouts,
		    d.voted ? d.voted_rails : "-", fm.ps_count, fm.rt_count);
		return;
	} else if (strcmp(cmd, "debug") == 0) {
		bt.debug = arg != NULL ? atoi(arg) : 1;
	} else if (strcmp(cmd, "watch") == 0) {
		c->watch = true;
	} else {
		snprintf(out, size, "error EINVAL unknown request %s", cmd);
		return;
	}
	if (error == 0) {
		snprintf(out, size, "ok");
		return;
	}
fail:
	snprintf(out, size, "error %s %s", errname(error),
	    error == -QFM_ENOSTATION ? "no station found" : strerror(-error));
}

/* --- the socket -------------------------------------------------------------- */

static int
listen_socket(void)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	int fd;

	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", d.socket);
	unlink(d.socket);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
	    bind(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0 ||
	    listen(fd, 4) != 0) {
		logmsg("%s: %s", d.socket, strerror(errno));
		return (-1);
	}
	chmod(d.socket, 0666);
	return (fd);
}

static void
client_input(struct client *c)
{
	char out[512], *nl;
	ssize_t n;

	n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
	if (n <= 0) {
		close(c->fd);
		c->fd = -1;
		return;
	}
	c->len += (size_t)n;
	c->buf[c->len] = '\0';
	while ((nl = strchr(c->buf, '\n')) != NULL) {
		*nl = '\0';
		if (nl > c->buf && nl[-1] == '\r')
			nl[-1] = '\0';
		out[0] = '\0';
		request(c, c->buf, out, sizeof(out) - 1);
		strcat(out, "\n");
		if (write(c->fd, out, strlen(out)) < 0) {
			close(c->fd);
			c->fd = -1;
			return;
		}
		c->len -= (size_t)(nl + 1 - c->buf);
		memmove(c->buf, nl + 1, c->len + 1);
	}
	if (c->len == sizeof(c->buf) - 1)
		c->len = 0;		/* a line too long: dropped */
}

static void
on_signal(int sig)
{
	(void)sig;
	d.quit = 1;
}

static void
usage(void)
{
	fprintf(stderr, "usage: pro1x-fmd [-Fv] [-d tty] [-s speed] "
	    "[-f fwdir:...] [-r rail,...|none] [-S socket]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct pollfd pfd[2 + MAX_CLIENTS];
	int ch, ls, i, n, timeout, fd, verbose = 0;

	d.t0 = mono_ms();
	while ((ch = getopt(argc, argv, "Fd:f:r:s:S:v")) != -1) {
		switch (ch) {
		case 'F':	d.foreground = true; break;
		case 'd':	d.tty = optarg; break;
		case 'f':	d.fwdirs = optarg; break;
		case 'r':	d.rails = optarg; break;
		case 's':	d.speed = (unsigned)strtoul(optarg, NULL, 10); break;
		case 'S':	d.socket = optarg; break;
		case 'v':	verbose++; break;
		default:	usage();
		}
	}
	if (d.speed != 115200 && d.speed != 3000000 && d.speed != 921600)
		usage();
	qbt_init(&bt, &ops, NULL, d.speed);
	bt.debug = verbose;
	qfm_init(&fm, &bt);
	fm.changed = fm_changed;
	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if ((ls = listen_socket()) < 0)
		return (1);
	if (!d.foreground && daemon(0, 1) != 0)
		return (1);
	logmsg("listening on %s, UART %s at %u baud", d.socket, d.tty,
	    d.speed);

	while (!d.quit) {
		n = 0;
		pfd[n++] = (struct pollfd){ .fd = ls, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = d.fd, .events = POLLIN };
		for (i = 0; i < MAX_CLIENTS; i++)
			pfd[n++] = (struct pollfd){ .fd = clients[i].fd,
			    .events = POLLIN };
		timeout = bt.up ? qbt_tick(&bt) : -1;
		if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
			break;
		if (bt.up && (pfd[1].revents & (POLLIN | POLLHUP)))
			qbt_pump(&bt, 0);
		else if (d.fd >= 0 && (pfd[1].revents & POLLIN))
			o_flush_input(NULL);
		if (pfd[0].revents & POLLIN) {
			fd = accept(ls, NULL, NULL);
			for (i = 0; fd >= 0 && i < MAX_CLIENTS; i++)
				if (clients[i].fd < 0) {
					clients[i] = (struct client){ .fd = fd };
					fd = -1;
				}
			if (fd >= 0)
				close(fd);
		}
		for (i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0 &&
			    (pfd[2 + i].revents & (POLLIN | POLLHUP)))
				client_input(&clients[i]);
	}
	logmsg("stopping");
	radio_off();
	close(ls);
	unlink(d.socket);
	return (0);
}
