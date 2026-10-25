/*
 * libbmeipc.so.0 for the Pro1X: the N9's battery management (BME) client
 * library, answered from the kernel's power supply class instead of the
 * N9's BME server (/tmp/.bmesrv), which needs the N9's charger hardware.
 *
 * Harmattan reads the battery through this library: qmsystem2's QmBattery
 * (bmeipc_open/query/eopen/eread) and contextkit's battery plugin
 * (bmeipc_open/stat/eopen, reading the event descriptor itself), which
 * gives the status bar its Battery.* properties.
 *
 * Statistics (BME_SYSMSG_GETSTAT, 32 ints), by index - taken from how
 * qmsystem2 and the battery plugin read them:
 *
 *    1 charger connected (1)       2 charger type (0 none, 3 wall)
 *    3 charging (1)                5 minutes until full
 *    6 battery state (0 empty, 1 low, 2 ok, 3 full)
 *    8 bars max                    9 bars now
 *   11 minutes until empty        12 nominal capacity (mAh)
 *   13 remaining capacity (mAh)   15 voltage (mV)
 *   16 charge (%)                 18 current (mA)
 *   19 coulomb counter (mAh)      22 condition (0 good)
 *
 * Events: as on the N9, bmeipc_eopen() returns an inotify instance watching
 * /tmp/.bmeevt, which the server rewrites on every change - here bmed, the
 * same source built with -DBMED, on Leste. bmeipc_eread() turns a change
 * into the BMEVENT bits QmBattery tests: 0x20 charging, 0x40 charger, 0x80
 * battery level, 0x10 battery state; 2 for no change, 1 for an error.
 *
 * Everything else the original exported fails with ENOSYS.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

#define PSY "/sys/class/power_supply"
#define GETSTAT 0x8003
#define NSTAT 32
#define BARS 8
#define CHECK_MS 60000

enum {
	CHARGER_STATE = 1, CHARGER_TYPE = 2, CHARGING_STATE = 3, CHARGING_TIME = 5,
	BATTERY_STATE = 6, LEVEL_MAX = 8, LEVEL_NOW = 9, TIME_LEFT = 11,
	CAPA_MAX = 12, CAPA_NOW = 13, VOLT_NOW = 15, LEVEL_PCT = 16, CURRENT = 18,
	COULOMB_COUNTER = 19, CONDITION = 22,
};

enum { EV_ERROR = 1, EV_NONE = 2, EV_STATE = 0x10, EV_CHARGE = 0x20, EV_CHARGER = 0x40, EV_BATMON = 0x80 };

/* --- statistics ----------------------------------------------------------------- */

static int read_attr(const char *dev, const char *attr, char *buf, size_t len)
{
	char path[384];
	snprintf(path, sizeof(path), PSY "/%s/%s", dev, attr);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static long read_long(const char *dev, const char *attr, long def)
{
	char buf[32];
	return read_attr(dev, attr, buf, sizeof(buf)) ? def : strtol(buf, NULL, 10);
}

/* The first power supply of the given type ("Battery", "Mains", "USB"). */
static int find_psy(const char *type, int online, char *name, size_t len)
{
	DIR *d = opendir(PSY);
	struct dirent *e;
	char buf[32];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		if (e->d_name[0] == '.' || read_attr(e->d_name, "type", buf, sizeof(buf)) || strcmp(buf, type))
			continue;
		if (online && read_long(e->d_name, "online", 0) != 1)
			continue;
		snprintf(name, len, "%s", e->d_name);
		found = 1;
	}
	closedir(d);
	return found;
}

static void read_stat(int *st)
{
	char bat[128], status[32] = "";

	memset(st, 0, NSTAT * sizeof(int));
	st[LEVEL_MAX] = BARS;
	if (!find_psy("Battery", 0, bat, sizeof(bat)))
		return;
	read_attr(bat, "status", status, sizeof(status));

	int pct = read_long(bat, "capacity", 0);
	long full_uah = read_long(bat, "charge_full_design", 0);
	long cur_ua = read_long(bat, "current_now", 0);
	int charging = !strcmp(status, "Charging");
	int full = !strcmp(status, "Full");
	char other[128];
	int charger = charging || full || !strcmp(status, "Not charging") ||
		      find_psy("Mains", 1, other, sizeof(other)) || find_psy("USB", 1, other, sizeof(other));

	st[CHARGER_STATE] = charger;
	st[CHARGER_TYPE] = charger ? 3 : 0;
	st[CHARGING_STATE] = charging;
	st[LEVEL_PCT] = pct;
	st[LEVEL_NOW] = (pct * BARS + 99) / 100;
	st[BATTERY_STATE] = full || pct >= 100 ? 3 : pct <= 2 ? 0 : pct <= 10 ? 1 : 2;
	st[CAPA_MAX] = full_uah / 1000;
	st[CAPA_NOW] = full_uah / 1000 * pct / 100;
	st[COULOMB_COUNTER] = st[CAPA_NOW];
	st[VOLT_NOW] = read_long(bat, "voltage_now", 0) / 1000;
	st[CURRENT] = cur_ua / 1000;
	st[CONDITION] = 0;

	/* the driver's sign convention varies: the status gives the direction */
	long ma = labs(cur_ua) / 1000;
	if (ma >= 10) {
		if (charging)
			st[CHARGING_TIME] = (st[CAPA_MAX] - st[CAPA_NOW]) * 60 / ma;
		else if (!charger)
			st[TIME_LEFT] = st[CAPA_NOW] * 60 / ma;
	}
}

/* What the event bits are about, as bmeipc_eread compares it. */
static void event_key(int *key)
{
	int st[NSTAT];
	read_stat(st);
	key[0] = st[CHARGING_STATE];
	key[1] = st[CHARGER_STATE];
	key[2] = st[LEVEL_NOW];
	key[3] = st[BATTERY_STATE];
}

/* --- requests ------------------------------------------------------------------- */

EXPORT int bmeipc_open(void)
{
	return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

EXPORT int bmeipc_close(int sd)
{
	return sd >= 0 ? close(sd) : 0;
}

EXPORT int bmeipc_query(int sd, const void *msg, int len, void *reply, int rlen)
{
	(void)sd;
	if (!msg || len < 2 || *(const uint16_t *)msg != GETSTAT || !reply || rlen <= 0) {
		errno = EINVAL;
		return -1;
	}
	int st[NSTAT];
	read_stat(st);
	if (rlen > (int)sizeof(st))
		rlen = sizeof(st);
	memcpy(reply, st, rlen);
	return rlen;
}

EXPORT int bmeipc_stat(int sd, void *stat)
{
	uint16_t req[2] = { GETSTAT, 0 };
	return bmeipc_query(sd, req, sizeof(req), stat, NSTAT * sizeof(int));
}

/* --- events --------------------------------------------------------------------- */

/*
 * Like the N9's: an inotify instance on the event file, which the BME
 * server (here bmed) rewrites whenever something changed. contextkit's
 * battery plugin adds its own watch on the same file to the returned
 * descriptor and reads the inotify events itself.
 */
#define EVENT_FILE "/tmp/.bmeevt"
#define EVENT_MASK (IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF)
#define MAX_FD 1024

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int reported[MAX_FD][4];

static void write_event_file(const char *path, const int *key)
{
	static int seq;
	int data[5] = { key[0], key[1], key[2], key[3], ++seq };
	/* /tmp is sticky: with fs.protected_regular, O_CREAT on a file of
	 * another user fails even for root */
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0 && errno == ENOENT)
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (fd < 0)
		return;
	fchmod(fd, 0666);
	if (write(fd, data, sizeof(data)) < 0)
		perror(path);
	close(fd);
}

EXPORT int bmeipc_eopen(int mask)
{
	(void)mask;
	if (access(EVENT_FILE, F_OK)) {
		int key[4];
		event_key(key);
		write_event_file(EVENT_FILE, key);
	}
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (fd < 0)
		return -1;
	if (fd >= MAX_FD || inotify_add_watch(fd, EVENT_FILE, EVENT_MASK) < 0) {
		close(fd);
		errno = fd >= MAX_FD ? EMFILE : errno;
		return -1;
	}
	pthread_mutex_lock(&lock);
	event_key(reported[fd]);
	pthread_mutex_unlock(&lock);
	return fd;
}

EXPORT int bmeipc_eread(int sd)
{
	char buf[4096];
	int now[4], ev = 0;
	static const int bits[4] = { EV_CHARGE, EV_CHARGER, EV_BATMON, EV_STATE };

	if (sd < 0 || sd >= MAX_FD)
		return EV_ERROR;
	while (read(sd, buf, sizeof(buf)) > 0)
		;
	event_key(now);
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 4; i++)
		if (now[i] != reported[sd][i])
			ev |= bits[i];
	memcpy(reported[sd], now, sizeof(now));
	pthread_mutex_unlock(&lock);
	return ev ? ev : EV_NONE;
}

EXPORT int bmeipc_eclose(int sd)
{
	return sd >= 0 ? close(sd) : 0;
}

#ifdef BMED
/*
 * bmed <event file>: the server side, on Leste. Rewrites the event file
 * when a uevent of the power supply class, or a check every minute, finds
 * a change.
 */
int main(int argc, char **argv)
{
	int key[4], now[4];
	char buf[4096];
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };

	if (argc != 2) {
		fprintf(stderr, "usage: %s <event file>\n", argv[0]);
		return 2;
	}
	int nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (nl >= 0 && bind(nl, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("uevent");
		close(nl);
		nl = -1;
	}
	event_key(key);
	write_event_file(argv[1], key);
	for (;;) {
		struct pollfd p = { .fd = nl, .events = POLLIN };
		int n = poll(&p, 1, CHECK_MS);
		if (n < 0 && errno != EINTR)
			return 1;
		int psy = n == 0;
		ssize_t len;
		while (nl >= 0 && (len = recv(nl, buf, sizeof(buf) - 1, 0)) > 0) {
			buf[len] = 0;
			for (char *s = buf; s < buf + len; s += strlen(s) + 1)
				if (!strcmp(s, "SUBSYSTEM=power_supply"))
					psy = 1;
		}
		if (!psy)
			continue;
		event_key(now);
		if (memcmp(now, key, sizeof(key))) {
			memcpy(key, now, sizeof(key));
			write_event_file(argv[1], key);
		}
	}
}
#endif

/* --- the rest of the original's interface ------------------------------------- */

#define UNSUPPORTED(name) \
	EXPORT int name() { errno = ENOSYS; return -1; }

UNSUPPORTED(bmeipc_send)
UNSUPPORTED(bmeipc_recv)
UNSUPPORTED(bmeipc_reply)
UNSUPPORTED(bmeipc_poll)
UNSUPPORTED(bmeipc_epoll)
UNSUPPORTED(bmeipc_recv_status)
UNSUPPORTED(bmeipc_getcookie)
UNSUPPORTED(bmeipc_putcookie)
UNSUPPORTED(bmeipc_getpid)
UNSUPPORTED(bme_connect)
UNSUPPORTED(bme_disconnect)
UNSUPPORTED(bme_read)
UNSUPPORTED(bme_write)
UNSUPPORTED(bme_packet_read)
UNSUPPORTED(bme_packet_write)
UNSUPPORTED(bme_send_get_reply)
UNSUPPORTED(bme_get_server_pid)
UNSUPPORTED(bmelog_init)
UNSUPPORTED(bmelog_exit)
UNSUPPORTED(bmelog_level)
UNSUPPORTED(bmelog_level_get)
UNSUPPORTED(bmelog_charge_mode)
UNSUPPORTED(bmelog_getlog)
UNSUPPORTED(bmelog_sendtrace)
UNSUPPORTED(bmelog_dumphex)
UNSUPPORTED(bmelog_getenv)

EXPORT char *bmeipc_getenv(const char *name)
{
	return getenv(name);
}
