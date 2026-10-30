/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pro1x-cam: the F(x)tec Pro1X's cameras under FreeBSD
 * (freebsd/camera/README.md). /dev/camss0 (qcom_camss.ko) has the SoC's
 * part; this has the board's and the sensors':
 *
 *   pro1x-cam info                 what the device tree says, the clocks
 *   pro1x-cam id [CAM...]          power up, read the chip ID over the CCI
 *                                  (and the rear lens' status), power down
 *   pro1x-cam power CAM on|off     power only (for i2c(8) by hand)
 *   pro1x-cam reg CAM REG [VAL]    a 16-bit register: read, or write VAL
 *                                  (-8: an 8-bit one); the camera powered
 *
 * CAM is rear, front or the index in "info". -v: every step.
 */

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cam.h"
#include "sensor_tables.h"

int verbose;

void
msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

void
vmsg(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	printf("  ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

void
msleep(u_int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: pro1x-cam [-v] [-d /dev/camss0] info\n"
	    "       pro1x-cam [-v] id [CAM ...]\n"
	    "       pro1x-cam [-v] power CAM on|off\n"
	    "       pro1x-cam [-v] reg [-8] CAM REG [VALUE]\n"
	    "CAM: rear, front or an index\n");
	exit(2);
}

static const char *const supply_type[] = { "other", "pca9534", "pm8008" };

static void
print_supply(const char *what, const struct camss_supply *s)
{
	printf("    %s %-7s %-22s %s", what, s->name, s->node,
	    supply_type[s->type < 3 ? s->type : 0]);
	if (s->type != CAMSS_SUPPLY_OTHER)
		printf(" %s%u @0x%02x iic%d", s->type == CAMSS_SUPPLY_PM8008 ?
		    "ldo" : "pin ", s->index, s->addr, s->iic_unit);
	if (s->min_uv)
		printf(" %u-%u uV", s->min_uv, s->max_uv);
	printf("\n");
}

static int
cmd_info(struct cam *c)
{
	const struct camss_sensor *cs;
	u_int s, i;

	printf("PM8008 reset gpio%d%s\n", c->info.pmic_reset_gpio,
	    c->info.pmic_reset_active_low ? " (active low)" : "");
	for (s = 0; s < c->info.nsensors; s++) {
		cs = &c->info.sensor[s];
		printf("%u %s: %s (%s) port/CSIPHY %u, %u lanes, CCI master %u "
		    "= iic%d @0x%02x\n", s, cam_name(c, s), cs->compatible,
		    cs->node, cs->port, cs->nlanes, cs->cci_master,
		    cs->iic_unit, cs->addr);
		printf("    reset gpio%d%s, MCLK%d %u Hz, rotation %d, link",
		    cs->reset_gpio, cs->reset_active_low ? " (active low)" : "",
		    cs->mclk, cs->mclk_hz, cs->rotation);
		for (i = 0; i < cs->nlink; i++)
			printf(" %ju", (uintmax_t)cs->link_freq[i]);
		printf(" Hz, driver %s\n", sensor_ops(c, s) ?
		    sensor_ops(c, s)->name : "none");
		for (i = 0; i < cs->nsupplies; i++)
			print_supply("rail", &cs->supply[i]);
		if (cs->has_lens) {
			printf("    lens %s (%s) @0x%02x\n", cs->lens_compatible,
			    cs->lens_node, cs->lens_addr);
			for (i = 0; i < cs->nlens_supplies; i++)
				print_supply("lens rail", &cs->lens_supply[i]);
		}
	}
	return (0);
}

static int
cmd_id(struct cam *c, int argc, char **argv)
{
	const struct sensor_ops *o;
	int list[CAMSS_MAX_SENSORS], n = 0, i, s, bad = 0;
	uint16_t id;
	uint8_t st;

	if (argc == 0)
		for (s = 0; s < (int)c->info.nsensors; s++)
			list[n++] = s;
	for (i = 0; i < argc && n < CAMSS_MAX_SENSORS; i++) {
		if ((s = cam_find(c, argv[i])) < 0)
			errx(2, "no camera %s", argv[i]);
		list[n++] = s;
	}
	for (i = 0; i < n; i++) {
		s = list[i];
		o = sensor_ops(c, s);
		msg("--- %s: %s at 0x%02x on CCI master %u (iic%d)", cam_name(c, s),
		    c->info.sensor[s].compatible, c->info.sensor[s].addr,
		    c->info.sensor[s].cci_master, c->info.sensor[s].iic_unit);
		if (sensor_power(c, s, 1) < 0) {
			msg("%s: power up failed", cam_name(c, s));
			bad++;
			continue;
		}
		if (sensor_read_id(c, s, &id) < 0) {
			msg("%s: chip ID register %#06x: no answer (%s)",
			    cam_name(c, s), o->id_reg, strerror(errno));
			bad++;
		} else if (id != o->id) {
			msg("%s: chip ID %#06x, %s expects %#06x", cam_name(c, s),
			    id, o->name, o->id);
			bad++;
		} else
			msg("%s: %s chip ID %#06x: ok", cam_name(c, s), o->name, id);
		if (c->info.sensor[s].has_lens) {
			if (lens_power(c, s, 1) < 0 || lens_status(c, s, &st) < 0) {
				msg("%s: lens %s at 0x%02x: no answer",
				    cam_name(c, s), c->info.sensor[s].lens_compatible,
				    c->info.sensor[s].lens_addr);
				bad++;
			} else
				msg("%s: lens %s at 0x%02x: status %#04x: ok",
				    cam_name(c, s), c->info.sensor[s].lens_compatible,
				    c->info.sensor[s].lens_addr, st);
			lens_power(c, s, 0);
		}
		sensor_power(c, s, 0);
	}
	return (bad ? 1 : 0);
}

int
main(int argc, char **argv)
{
	const char *dev = "/dev/camss0";
	struct cam c;
	uint32_t v;
	int ch, s, width = 16;

	while ((ch = getopt(argc, argv, "d:v8")) != -1) {
		switch (ch) {
		case 'd':
			dev = optarg;
			break;
		case 'v':
			verbose++;
			break;
		case '8':
			width = 8;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();
	if (cam_open(&c, dev) < 0)
		err(1, "%s", dev);
	if (strcmp(argv[0], "info") == 0)
		return (cmd_info(&c));
	if (strcmp(argv[0], "id") == 0)
		return (cmd_id(&c, argc - 1, argv + 1));
	if (strcmp(argv[0], "power") == 0 && argc == 3) {
		if ((s = cam_find(&c, argv[1])) < 0)
			errx(2, "no camera %s", argv[1]);
		/* the process ends here: rails stay as they are left */
		return (sensor_power(&c, s, strcmp(argv[2], "on") == 0) < 0);
	}
	if (strcmp(argv[0], "reg") == 0 && (argc == 3 || argc == 4)) {
		if ((s = cam_find(&c, argv[1])) < 0)
			errx(2, "no camera %s", argv[1]);
		if (argc == 4) {
			v = strtoul(argv[3], NULL, 0);
			if (sensor_reg(&c, s, strtoul(argv[2], NULL, 0), width, 1,
			    &v) < 0)
				err(1, "write");
			return (0);
		}
		if (sensor_reg(&c, s, strtoul(argv[2], NULL, 0), width, 0, &v) < 0)
			err(1, "read");
		printf("%#x\n", v);
		return (0);
	}
	usage();
}
