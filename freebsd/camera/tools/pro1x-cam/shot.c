/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * One picture (stage 3, freebsd/camera/README.md): exposure, focus (the
 * contrast sweep of pro1x-camera's findFocus()), the frame demosaicked,
 * turned and written as JPEG; for pro1x-cam's "shot" and the camera app.
 */

#include <err.h>
#include <stdlib.h>
#include <string.h>

#include "cam.h"
#include "sensor_tables.h"

void
shot_defaults(struct shot_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->quality = 90;
	o->focus = -1;
	o->rotation = -1000;
	o->small = 1;
}


struct shotctx {
	const struct sensor_ops	*o;
	const struct sensor_mode *m;
	const char		*out;
	int			quality, rotation, af, lens, small;
	uint16_t		*px;
	/* exposure */
	u_int			lines, gain, settle, ae_frames;
	double			again;
	/* focus: the sweep's positions, the current one, the best */
	int			pos[24], npos, ipos, wait, best, focus;
	double			bestv;
	int			done;
};

/* aim the green mean (above black) at 18 % of the range */
static int
ae_step(struct cam *c, int s, struct shotctx *x, double mean)
{
	const double target = 0.18 * (1023 - 64);
	u_int max = x->m->vts_def - x->o->exp_margin;
	double want;

	if (mean > target * 0.8 && mean < target * 1.25)
		return (1);		/* close enough */
	want = x->lines * x->again * target / (mean > 1 ? mean : 1);
	if (want <= max) {
		x->lines = want < x->o->exp_min ? x->o->exp_min : (u_int)want;
		x->again = 1;
	} else {
		x->lines = max;
		x->again = want / max > 16 ? 16 : want / max;
	}
	x->gain = sensor_gain_code(x->o, x->again);
	vmsg("AE: mean %.1f -> exposure %u lines, gain %.2f (code %u)", mean,
	    x->lines, x->again, x->gain);
	x->o->exposure(c, s, x->m, x->lines, x->gain);
	return (0);
}

static int
shot_frame(struct cam *c, int s, struct frame *f, void *arg)
{
	struct shotctx *x = arg;
	double mean[4], clipped, v;
	uint32_t maxv, w, h;
	uint8_t *rgb;
	char path[256], *dot;
	FILE *fp;

	raw10_unpack(f, x->px);
	if (x->settle > 0) {
		x->settle--;
		return (0);
	}
	/* exposure first: a few rounds, two frames each for it to apply */
	if (x->ae_frames < 12) {
		raw_stats(x->px, f->width, f->height, x->o->bayer, mean, &maxv,
		    &clipped);
		x->ae_frames++;
		if (ae_step(c, s, x, (mean[0] + mean[1] + mean[2] + mean[3]) / 4 -
		    64) == 0) {
			x->settle = 2;
			return (0);
		}
		x->ae_frames = 12;
		msg("%s: exposure %u lines, gain %.2f", cam_name(c, s), x->lines,
		    x->again);
	}
	/* then focus: pro1x-camera's findFocus(), one frame dropped after
	 * each move */
	if (x->af && x->lens && x->ipos < x->npos) {
		if (x->wait) {
			x->wait = 0;
			return (0);
		}
		if (x->ipos >= 0 && x->focus >= 0) {
			v = sharpness(x->px, f->width, f->height);
			vmsg("AF: %d: %.0f", x->focus, v);
			if (v > x->bestv) {
				x->bestv = v;
				x->best = x->focus;
			}
		}
		if (++x->ipos == 11) {
			/* the fine sweep around the coarse best */
			int p, n = 11;
			for (p = x->best - 80; p <= x->best + 80; p += 20)
				if (p != x->best && p >= 0 && p <= 1023 && n < 24)
					x->pos[n++] = p;
			x->npos = n;
		}
		if (x->ipos < x->npos) {
			lens_move(c, s, x->focus, x->pos[x->ipos]);
			x->focus = x->pos[x->ipos];
			x->wait = 1;
			return (0);
		}
		lens_move(c, s, x->focus, x->best);
		x->focus = x->best;
		msg("%s: focus %d (contrast %.0f)", cam_name(c, s), x->best,
		    x->bestv);
		x->settle = 2;
		return (0);
	}
	/* the picture */
	rgb = debayer(x->px, f->width, f->height, x->o->bayer, 64, 0, &w, &h);
	if (rgb == NULL) {
		warnx("debayer: no memory");
		return (-1);
	}
	rotate_rgb(&rgb, &w, &h, x->rotation);
	if ((fp = fopen(x->out, "w")) == NULL ||
	    jpeg_write(fp, rgb, w, h, x->quality) < 0 || fclose(fp) != 0) {
		warn("%s", x->out);
		free(rgb);
		return (-1);
	}
	msg("%s: frame %u as %s: %ux%u, quality %d", cam_name(c, s),
	    f->sequence, x->out, w, h, x->quality);
	/* and at half size, small enough for /dev/pro1xshot */
	if (!x->small) {
		free(rgb);
		x->done = 1;
		return (1);
	}
	snprintf(path, sizeof(path), "%s", x->out);
	if ((dot = strrchr(path, '.')) != NULL)
		*dot = '\0';
	strlcat(path, "-small.jpg", sizeof(path));
	free(rgb);
	rgb = debayer(x->px, f->width, f->height, x->o->bayer, 64, 1, &w, &h);
	if (rgb != NULL) {
		rotate_rgb(&rgb, &w, &h, x->rotation);
		if ((fp = fopen(path, "w")) == NULL ||
		    jpeg_write(fp, rgb, w, h, 85) < 0 || fclose(fp) != 0)
			warn("%s", path);
		free(rgb);
	}
	x->done = 1;
	return (1);
}

int
take_picture(struct cam *c, int s, struct shot_opts *o)
{
	struct shotctx x;
	int error, p;

	memset(&x, 0, sizeof(x));
	x.out = o->out;
	x.quality = o->quality;
	x.af = o->af;
	x.focus = o->focus;
	x.small = o->small;
	if ((x.o = sensor_ops(c, s)) == NULL || (x.m = sensor_mode(x.o,
	    o->mode)) == NULL) {
		warnx("no such camera or mode");
		return (-1);
	}
	/* pro1x-camera's rotations for Hildon's landscape: rear 180, front 0 */
	x.rotation = o->rotation != -1000 ? o->rotation :
	    (c->info.sensor[s].orientation == 1 ? 180 : 0);
	if ((x.px = malloc((size_t)x.m->width * x.m->height * 2)) == NULL)
		return (-1);
	if (sensor_power(c, s, 1) < 0) {
		free(x.px);
		return (-1);
	}
	if (c->info.sensor[s].has_lens && lens_power(c, s, 1) == 0) {
		x.lens = 1;
		if (x.focus < 0)
			x.focus = 100;	/* dw9800's default_pos, about 1 m */
		lens_move(c, s, 0, x.focus);
	}
	/* the sensor's defaults: maximum exposure, minimum gain */
	x.lines = x.m->vts_def - x.o->exp_margin;
	x.again = 1;
	x.gain = x.o->gain_min;
	x.settle = 2;
	for (p = 0; p <= 1000; p += 100)
		x.pos[x.npos++] = p;
	x.ipos = -1;
	x.best = x.focus;
	x.bestv = -1;
	error = capture(c, s, x.m, 0, 200, shot_frame, &x);
	if (x.lens) {
		lens_move(c, s, x.focus, 0);	/* parked gently */
		lens_power(c, s, 0);
	}
	sensor_power(c, s, 0);
	free(x.px);
	o->focus = x.focus;
	return (error < 0 || !x.done ? -1 : 0);
}
