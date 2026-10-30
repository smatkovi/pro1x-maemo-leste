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
 *   pro1x-cam raw [-m MODE] [-n N] [-o PREFIX] CAM
 *                                  stage 2: N raw frames, statistics of
 *                                  each, the last kept (PREFIX.raw10, .txt,
 *                                  -preview.png)
 *   pro1x-cam shot [-a] [-f POS] [-m MODE] [-q Q] [-r DEG] -o FILE.jpg CAM
 *                                  stage 3: exposure, focus (-a: the
 *                                  contrast sweep of pro1x-camera), one
 *                                  frame, debayered, rotated, as JPEG
 *                                  (and FILE-small.jpg at half size)
 *   pro1x-cam focus CAM POS        the rear lens to POS (0-1023), its status
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

static void __dead2
usage(void)
{
	fprintf(stderr,
	    "usage: pro1x-cam [-v] [-d /dev/camss0] info\n"
	    "       pro1x-cam [-v] id [CAM ...]\n"
	    "       pro1x-cam [-v] power CAM on|off\n"
	    "       pro1x-cam [-v] reg [-8] CAM REG [VALUE]\n"
	    "       pro1x-cam [-v] raw [-m MODE] [-n N] [-o PREFIX] CAM\n"
	    "       pro1x-cam [-v] shot [-a] [-f POS] [-m MODE] [-q Q] [-r DEG] -o FILE CAM\n"
	    "       pro1x-cam [-v] focus CAM POS\n"
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

/* --- stage 2: raw frames ------------------------------------------------------ */

struct rawctx {
	const char		*prefix;
	const struct sensor_ops	*o;
	const struct sensor_mode *m;
	u_int			n, seen;
	uint16_t		*px;
};

static int
raw_frame(struct cam *c, int s, struct frame *f, void *arg)
{
	struct rawctx *r = arg;
	double mean[4], clipped;
	uint32_t maxv, ow, oh;
	char path[256];
	uint8_t *rgb;
	FILE *fp;

	raw10_unpack(f, r->px);
	raw_stats(r->px, f->width, f->height, r->o->bayer, mean, &maxv,
	    &clipped);
	msg("%s: frame %u: means %s %.1f %.1f %.1f %.1f, max %u, %.2f%% "
	    "saturated%s", cam_name(c, s), f->sequence, r->o->bayer, mean[0],
	    mean[1], mean[2], mean[3], maxv, 100 * clipped,
	    f->flags & CAMSS_BUF_ERROR ? ", bus error" : "");
	if (++r->seen < r->n || r->prefix == NULL)
		return (0);
	/* the last one, as it is, and what it shows */
	snprintf(path, sizeof(path), "%s.raw10", r->prefix);
	if ((fp = fopen(path, "w")) == NULL ||
	    fwrite(f->data, f->stride, f->height, fp) != f->height ||
	    fclose(fp) != 0)
		warn("%s", path);
	snprintf(path, sizeof(path), "%s.txt", r->prefix);
	if ((fp = fopen(path, "w")) != NULL) {
		fprintf(fp, "width %u height %u stride %u bayer %s sequence %u "
		    "sensor %s mode %s black 64 mean %.1f/%.1f/%.1f/%.1f max %u "
		    "saturated %.4f\n", f->width, f->height, f->stride,
		    r->o->bayer, f->sequence, r->o->name, r->m->name, mean[0],
		    mean[1], mean[2], mean[3], maxv, clipped);
		fclose(fp);
	}
	/* a preview: one pixel per 2x2 quad, then halved again if large */
	rgb = debayer(r->px, f->width, f->height, r->o->bayer, 64, 1, &ow, &oh);
	if (rgb != NULL) {
		while (ow > 1100) {
			uint32_t x, y, nw = ow / 2, nh = oh / 2;
			int k;

			for (y = 0; y < nh; y++)
				for (x = 0; x < nw; x++)
					for (k = 0; k < 3; k++)
						rgb[(y * nw + x) * 3 + k] = (rgb[(2 * y *
						    ow + 2 * x) * 3 + k] + rgb[(2 * y * ow +
						    2 * x + 1) * 3 + k] + rgb[((2 * y + 1) *
						    ow + 2 * x) * 3 + k] + rgb[((2 * y + 1) *
						    ow + 2 * x + 1) * 3 + k] + 2) / 4;
			ow = nw;
			oh = nh;
		}
		snprintf(path, sizeof(path), "%s-preview.png", r->prefix);
		if ((fp = fopen(path, "w")) == NULL ||
		    png_write(fp, rgb, ow, oh, 3, 8) < 0 || fclose(fp) != 0)
			warn("%s", path);
		free(rgb);
	}
	msg("%s: kept frame %u in %s.raw10 (%ux%u, stride %u)", cam_name(c, s),
	    f->sequence, r->prefix, f->width, f->height, f->stride);
	return (1);
}

static const char *
default_mode(const struct sensor_ops *o, int raw)
{
	/* the raw run: the modes that fit /dev/pro1xshot (3 MiB) */
	if (raw)
		return (strcmp(o->name, "IMX582") == 0 ? "1920x1080" :
		    "1632x1224");
	return (NULL);		/* the first: full size */
}

static int
cmd_raw(struct cam *c, int argc, char **argv)
{
	struct rawctx r = { 0 };
	const char *mode = NULL;
	int ch, s, error;

	r.n = 8;
	optind = 1;
	optreset = 1;
	while ((ch = getopt(argc, argv, "m:n:o:")) != -1) {
		switch (ch) {
		case 'm':
			mode = optarg;
			break;
		case 'n':
			r.n = strtoul(optarg, NULL, 0);
			break;
		case 'o':
			r.prefix = optarg;
			break;
		default:
			usage();
		}
	}
	if (optind + 1 != argc || (s = cam_find(c, argv[optind])) < 0)
		usage();
	if ((r.o = sensor_ops(c, s)) == NULL)
		errx(1, "no driver for %s", c->info.sensor[s].compatible);
	if (mode == NULL)
		mode = default_mode(r.o, 1);
	if ((r.m = sensor_mode(r.o, mode)) == NULL)
		errx(2, "%s has no mode %s", r.o->name, mode);
	if ((r.px = malloc((size_t)r.m->width * r.m->height * 2)) == NULL)
		err(1, "malloc");
	if (sensor_power(c, s, 1) < 0)
		return (1);
	error = capture(c, s, r.m, 2, r.n, raw_frame, &r);
	sensor_power(c, s, 0);
	free(r.px);
	return (error < 0 || r.seen == 0);
}

/* --- stage 3: a picture (shot.c) -------------------------------------------- */

static int
cmd_shot(struct cam *c, int argc, char **argv)
{
	struct shot_opts o;
	int ch, s;

	shot_defaults(&o);
	optind = 1;
	optreset = 1;
	while ((ch = getopt(argc, argv, "af:m:o:q:r:")) != -1) {
		switch (ch) {
		case 'a':
			o.af = 1;
			break;
		case 'f':
			o.focus = strtol(optarg, NULL, 0);
			break;
		case 'm':
			o.mode = optarg;
			break;
		case 'o':
			o.out = optarg;
			break;
		case 'q':
			o.quality = strtol(optarg, NULL, 0);
			break;
		case 'r':
			o.rotation = strtol(optarg, NULL, 0);
			break;
		default:
			usage();
		}
	}
	if (optind + 1 != argc || o.out == NULL ||
	    (s = cam_find(c, argv[optind])) < 0)
		usage();
	return (take_picture(c, s, &o) < 0);
}

static int
cmd_focus(struct cam *c, int s, int pos)
{
	uint8_t st;
	int error;

	if (!c->info.sensor[s].has_lens)
		errx(2, "%s has no lens to move", cam_name(c, s));
	if (lens_power(c, s, 1) < 0)
		return (1);
	error = lens_move(c, s, 0, pos);
	if (lens_status(c, s, &st) == 0)
		msg("%s: lens at %d, status %#04x", cam_name(c, s), pos, st);
	lens_move(c, s, pos, 0);
	lens_power(c, s, 0);
	return (error < 0);
}

int
main(int argc, char **argv)
{
	const char *dev = "/dev/camss0";
	struct cam c;
	uint32_t v;
	int ch, s, width = 16;

	while ((ch = getopt(argc, argv, "+d:v8")) != -1) {
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
	if (strcmp(argv[0], "raw") == 0)
		return (cmd_raw(&c, argc, argv));
	if (strcmp(argv[0], "shot") == 0)
		return (cmd_shot(&c, argc, argv));
	if (strcmp(argv[0], "focus") == 0 && argc == 3) {
		if ((s = cam_find(&c, argv[1])) < 0)
			errx(2, "no camera %s", argv[1]);
		return (cmd_focus(&c, s, strtol(argv[2], NULL, 0)));
	}
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
