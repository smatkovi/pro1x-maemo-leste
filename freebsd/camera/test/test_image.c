/*
 * Host test of pro1x-cam's picture path: a synthetic scene (colour bars
 * and a gradient) mosaicked as each sensor's pattern, packed as RAW10 the
 * way the TFE writes CSI-2 RAW10 (stride 8-byte aligned), unpacked again
 * (exact), demosaicked (flat colour areas must keep their hue), written as
 * PNG and JPEG into test-out/ for check_image.py (which decodes them with
 * Pillow) and the focus contrast (a sharp edge must score higher than the
 * same texture blurred).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../tools/pro1x-cam/cam.h"

int verbose;
void msg(const char *fmt, ...) { (void)fmt; }
void vmsg(const char *fmt, ...) { (void)fmt; }

static int fails;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

/* the scene: 8 vertical bars (white, yellow, cyan, green, magenta, red,
 * blue, black) over the top half, a horizontal grey ramp below */
static int checker;	/* the focus test's scene: 4x4 squares */

static void
scene(uint32_t x, uint32_t y, uint32_t w, uint32_t h, double rgb[3])
{
	static const double bars[8][3] = { {1,1,1}, {1,1,0}, {0,1,1}, {0,1,0},
	    {1,0,1}, {1,0,0}, {0,0,1}, {0,0,0} };
	int i;

	if (checker) {
		for (i = 0; i < 3; i++)
			rgb[i] = ((x / 4 + y / 4) & 1) ? 0.9 : 0.1;
		return;
	}
	if (y < h / 2) {
		for (i = 0; i < 3; i++)
			rgb[i] = 0.05 + 0.85 * bars[x * 8 / w][i];
	} else
		for (i = 0; i < 3; i++)
			rgb[i] = 0.05 + 0.85 * x / w;
}

static uint8_t *
mosaic(uint32_t w, uint32_t h, const char *bayer, uint32_t stride, int blur,
    uint16_t *truth)
{
	uint8_t *raw = calloc(stride, h);
	uint32_t x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			double rgb[3] = { 0, 0, 0 }, t[3];
			char c = bayer[(y & 1) * 2 + (x & 1)];
			int dx, dy, n = 0;
			uint16_t v;

			/* blur: the scene averaged over a (2 blur + 1)^2 box */
			for (dy = -blur; dy <= blur; dy++)
				for (dx = -blur; dx <= blur; dx++) {
					int xx = (int)x + dx, yy = (int)y + dy;
					if (xx < 0 || yy < 0 || xx >= (int)w ||
					    yy >= (int)h)
						continue;
					scene(xx, yy, w, h, t);
					for (int k = 0; k < 3; k++)
						rgb[k] += t[k];
					n++;
				}
			for (int k = 0; k < 3; k++)
				rgb[k] /= n;
			v = (uint16_t)lrint(64 + 959 * rgb[c == 'R' ? 0 : c == 'G' ? 1 : 2]);
			truth[y * w + x] = v;
			raw[y * stride + x / 4 * 5 + x % 4] = v >> 2;
			raw[y * stride + x / 4 * 5 + 4] |= (v & 3) << (2 * (x % 4));
		}
	return (raw);
}

static int
write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
	FILE *fp = fopen(path, "w");
	int e = fp == NULL || png_write(fp, rgb, w, h, 3, 8) < 0;

	if (fp)
		fclose(fp);
	return (e);
}

int
main(void)
{
	static const char *const pats[] = { "RGGB", "GRBG", "BGGR", "GBRG" };
	const uint32_t w = 320, h = 240;
	uint32_t stride = (w * 10 / 8 + 7) & ~7u, ow, oh, i;
	uint16_t *truth = malloc(w * h * 2), *px = malloc(w * h * 2);
	struct frame f;
	char path[64];
	FILE *fp;
	u_int p;

	mkdir("test-out", 0755);
	for (p = 0; p < 4; p++) {
		uint8_t *raw = mosaic(w, h, pats[p], stride, 0, truth), *rgb;

		f.width = w;
		f.height = h;
		f.stride = stride;
		f.data = raw;
		memset(px, 0, w * h * 2);
		raw10_unpack(&f, px);
		CHECK(memcmp(px, truth, w * h * 2) == 0);
		for (int half = 0; half < 2; half++) {
			rgb = debayer(px, w, h, pats[p], 64, half, &ow, &oh);
			CHECK(rgb != NULL && ow == (half ? w / 2 : w) &&
			    oh == (half ? h / 2 : h));
			/* the red bar (6th) is red, the blue (7th) blue, the
			 * green (4th) green; the ramp grey */
			{
				const uint8_t *r = rgb + ((oh / 4) * ow +
				    ow * 5 / 8 + ow / 16) * 3;
				const uint8_t *b = rgb + ((oh / 4) * ow +
				    ow * 6 / 8 + ow / 16) * 3;
				const uint8_t *g = rgb + ((oh / 4) * ow +
				    ow * 3 / 8 + ow / 16) * 3;
				const uint8_t *k = rgb + ((oh * 3 / 4) * ow +
				    ow / 2) * 3;
				CHECK(r[0] > 2 * r[1] && r[0] > 2 * r[2]);
				CHECK(b[2] > 2 * b[0] && b[2] > 2 * b[1]);
				CHECK(g[1] > 2 * g[0] && g[1] > 2 * g[2]);
				CHECK(abs(k[0] - k[1]) < 12 && abs(k[1] - k[2]) < 12);
			}
			snprintf(path, sizeof(path), "test-out/%s-%s.png",
			    pats[p], half ? "half" : "full");
			CHECK(write_png(path, rgb, ow, oh) == 0);
			if (!half) {
				rotate_rgb(&rgb, &ow, &oh, 90);
				CHECK(ow == h && oh == w);
				rotate_rgb(&rgb, &ow, &oh, 270);
				CHECK(ow == w && oh == h);
				snprintf(path, sizeof(path), "test-out/%s.jpg",
				    pats[p]);
				fp = fopen(path, "w");
				CHECK(fp != NULL && jpeg_write(fp, rgb, ow, oh, 90) == 0);
				fclose(fp);
				/* the odd size: not a multiple of 16 */
				snprintf(path, sizeof(path), "test-out/%s-odd.jpg",
				    pats[p]);
				fp = fopen(path, "w");
				CHECK(jpeg_write(fp, rgb, ow - 5, oh - 3, 75) == 0);
				fclose(fp);
			}
			free(rgb);
		}
		free(raw);
	}
	/* the 16-bit grey PNG of the raw values */
	fp = fopen("test-out/raw16.png", "w");
	CHECK(fp != NULL && png_write(fp, (uint8_t *)truth, w, h, 1, 16) == 0);
	fclose(fp);
	/* focus contrast: sharp beats blurred (on texture: the sum of the
	 * differences of a monotonic edge does not change with blur) */
	checker = 1;
	{
		uint8_t *a = mosaic(w, h, "RGGB", stride, 0, truth);
		uint8_t *b = mosaic(w, h, "RGGB", stride, 3, truth);
		double sa, sb;

		f.data = a;
		raw10_unpack(&f, px);
		sa = sharpness(px, w, h);
		f.data = b;
		raw10_unpack(&f, px);
		sb = sharpness(px, w, h);
		(void)i;
		CHECK(sa > sb * 1.5);
		free(a);
		free(b);
	}
	free(truth);
	free(px);
	printf("test_image: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return (fails != 0);
}
