/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Raw frames to pictures on the CPU (FreeBSD has no ISP driver; under
 * Leste libcamera's software ISP does this on the GPU):
 *
 * - RAW10 as the TFE's RDI path writes it: the CSI-2 packing, 4 pixels in
 *   5 bytes (the high 8 bits each, then a byte with the 4 low 2-bit parts)
 * - the black level of the Leste tuning (libcamera's sensor helpers for
 *   imx582 and s5k4h7: 4096 at 16 bits = 64 at 10 bits)
 * - demosaicing: bilinear, or one RGB pixel per 2x2 Bayer quad (half size)
 * - white balance: grey world (libcamera's software ISP Awb has no
 *   calibration for these sensors either: the tuning files have no CCM)
 * - sRGB-like gamma 1/2.2, exposure normalised so that the 99.5th
 *   percentile is white
 * - PNG through zlib (base system); the JPEG encoder is jpeg.c
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "cam.h"

void
raw10_unpack(const struct frame *f, uint16_t *out)
{
	const uint8_t *l, *b;
	uint32_t x, y, i;

	for (y = 0; y < f->height; y++) {
		l = f->data + (size_t)y * f->stride;
		for (x = 0; x + 3 < f->width; x += 4) {
			b = l + x / 4 * 5;
			for (i = 0; i < 4; i++)
				out[(size_t)y * f->width + x + i] =
				    (uint16_t)(b[i] << 2 | ((b[4] >> (2 * i)) & 3));
		}
	}
}

/* the position of colour c ('R', 'G', 'B') in the 2x2 pattern */
static int
bayer_pos(const char *bayer, int (*pos)[2])
{
	int i, g = 0;

	for (i = 0; i < 4; i++) {
		switch (bayer[i]) {
		case 'R':
			pos[0][0] = i;
			break;
		case 'B':
			pos[2][0] = i;
			break;
		case 'G':
			pos[1][g++] = i;
			break;
		default:
			return (-1);
		}
	}
	return (g == 2 ? 0 : -1);
}

/* the mean of each of the four positions of the 2x2 pattern, the largest
 * value, the share of saturated pixels */
void
raw_stats(const uint16_t *px, uint32_t w, uint32_t h, const char *bayer,
    double mean[4], uint32_t *maxv, double *clipped)
{
	uint64_t sum[4] = { 0 }, n[4] = { 0 }, clip = 0, tot = 0;
	uint32_t x, y, v, m = 0;
	int i;

	for (y = 0; y < h; y += 1 + (h > 1000)) {
		for (x = 0; x < w; x += 1 + 2 * (w > 1000)) {
			v = px[(size_t)y * w + x];
			i = (y & 1) * 2 + (x & 1);
			sum[i] += v;
			n[i]++;
			tot++;
			if (v > m)
				m = v;
			if (v >= 1020)
				clip++;
		}
	}
	for (i = 0; i < 4; i++)
		mean[i] = n[i] ? (double)sum[i] / n[i] : 0;
	*maxv = m;
	*clipped = tot ? (double)clip / tot : 0;
	(void)bayer;
}

static uint16_t
at(const uint16_t *px, uint32_t w, uint32_t h, int x, int y)
{
	if (x < 0)
		x = -x;
	if (y < 0)
		y = -y;
	if (x >= (int)w)
		x = 2 * (int)w - 2 - x;
	if (y >= (int)h)
		y = 2 * (int)h - 2 - y;
	return (px[(size_t)y * w + x]);
}

/*
 * RGB, 8 bits a channel; half: one pixel per 2x2 quad (w/2 x h/2), else
 * bilinear at full size. NULL on an unknown pattern or no memory.
 */
uint8_t *
debayer(const uint16_t *px, uint32_t w, uint32_t h, const char *bayer,
    u_int black, int half, uint32_t *ow, uint32_t *oh)
{
	int pos[3][2], col[4];
	double mean[4], gain[3], scale, sum[3] = { 0, 0, 0 };
	uint32_t maxv, x, y, ww, hh, hist[1024], cnt, lim;
	uint8_t *out, lut[4096];
	float *lin = NULL;
	double clipped;
	size_t i, n;
	int c, k;

	if (bayer_pos(bayer, pos) < 0)
		return (NULL);
	for (k = 0; k < 4; k++)
		col[k] = bayer[k] == 'R' ? 0 : bayer[k] == 'G' ? 1 : 2;
	raw_stats(px, w, h, bayer, mean, &maxv, &clipped);
	for (k = 0; k < 4; k++)
		sum[col[k]] += (mean[k] > black ? mean[k] - black : 0.5) /
		    (col[k] == 1 ? 2 : 1);
	/* grey world */
	for (c = 0; c < 3; c++)
		gain[c] = sum[1] / (sum[c] > 0.5 ? sum[c] : 0.5);
	ww = half ? w / 2 : w;
	hh = half ? h / 2 : h;
	n = (size_t)ww * hh;
	if ((out = malloc(n * 3)) == NULL || (lin = malloc(n * 3 *
	    sizeof(float))) == NULL) {
		free(out);
		return (NULL);
	}
	for (y = 0; y < hh; y++) {
		for (x = 0; x < ww; x++) {
			float rgb[3] = { 0, 0, 0 };
			int xx = x, yy = y;

			if (half) {
				xx = 2 * x;
				yy = 2 * y;
				for (k = 0; k < 4; k++) {
					int v = at(px, w, h, xx + (k & 1),
					    yy + (k >> 1)) - (int)black;
					rgb[col[k]] += (v > 0 ? v : 0) /
					    (col[k] == 1 ? 2.0f : 1.0f);
				}
			} else {
				int here = (yy & 1) * 2 + (xx & 1);
				int cc = col[here];

				for (c = 0; c < 3; c++) {
					float s = 0;
					int m = 0, dy, dx;

					if (c == cc) {
						rgb[c] = at(px, w, h, xx, yy);
						continue;
					}
					for (dy = -1; dy <= 1; dy++)
						for (dx = -1; dx <= 1; dx++) {
							int p = ((yy + dy) & 1) * 2 +
							    ((xx + dx) & 1);
							if (col[p] == c && (dx || dy)) {
								s += at(px, w, h,
								    xx + dx, yy + dy);
								m++;
							}
						}
					rgb[c] = m ? s / m : 0;
				}
				for (c = 0; c < 3; c++)
					rgb[c] = rgb[c] > black ? rgb[c] - black : 0;
			}
			for (c = 0; c < 3; c++)
				lin[((size_t)y * ww + x) * 3 + c] = rgb[c] * gain[c];
		}
	}
	/* exposure: the 99.5th percentile of the brightest channel to white */
	memset(hist, 0, sizeof(hist));
	for (i = 0; i < n; i += 7) {
		float m = lin[i * 3];
		for (c = 1; c < 3; c++)
			if (lin[i * 3 + c] > m)
				m = lin[i * 3 + c];
		hist[m > 1023 ? 1023 : (uint32_t)m]++;
	}
	lim = (uint32_t)((n / 7 + 1) * 0.005);
	for (k = 1023, cnt = 0; k > 0 && cnt + hist[k] < lim; k--)
		cnt += hist[k];
	scale = 4095.0 / (k > 16 ? k : 16);
	for (k = 0; k < 4096; k++)
		lut[k] = (uint8_t)lrint(255.0 * pow(k / 4095.0, 1 / 2.2));
	for (i = 0; i < n * 3; i++) {
		double v = lin[i] * scale;
		out[i] = lut[v >= 4095 ? 4095 : (v <= 0 ? 0 : (int)v)];
	}
	free(lin);
	*ow = ww;
	*oh = hh;
	return (out);
}

/* clockwise by 0, 90, 180, 270 degrees */
void
rotate_rgb(uint8_t **rgb, uint32_t *w, uint32_t *h, int deg)
{
	uint32_t x, y, W = *w, H = *h, nx, ny, nw, nh;
	uint8_t *in = *rgb, *o;

	deg = ((deg % 360) + 360) % 360;
	if (deg == 0)
		return;
	nw = deg == 180 ? W : H;
	nh = deg == 180 ? H : W;
	if ((o = malloc((size_t)W * H * 3)) == NULL)
		return;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			if (deg == 90) {
				nx = H - 1 - y;
				ny = x;
			} else if (deg == 180) {
				nx = W - 1 - x;
				ny = H - 1 - y;
			} else {
				nx = y;
				ny = W - 1 - x;
			}
			memcpy(o + ((size_t)ny * nw + nx) * 3,
			    in + ((size_t)y * W + x) * 3, 3);
		}
	free(in);
	*rgb = o;
	*w = nw;
	*h = nh;
}

static void
be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static int
chunk(FILE *fp, const char *type, const uint8_t *d, uint32_t len)
{
	uint8_t b[8];
	uLong crc;

	be32(b, len);
	memcpy(b + 4, type, 4);
	crc = crc32(0, b + 4, 4);
	if (len)
		crc = crc32(crc, d, len);
	if (fwrite(b, 1, 8, fp) != 8 || (len && fwrite(d, 1, len, fp) != len))
		return (-1);
	be32(b, (uint32_t)crc);
	return (fwrite(b, 1, 4, fp) == 4 ? 0 : -1);
}

/* PNG: grey (1) or RGB (3) channels of 8 bits, or grey 16 (host order in) */
int
png_write(FILE *fp, const uint8_t *px, uint32_t w, uint32_t h, int channels,
    int bits)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a,
	    '\n' };
	uint8_t ihdr[13], *raw, *z;
	size_t bpr = (size_t)w * channels * (bits / 8), i, x;
	uLongf zlen;
	int error;

	if ((raw = malloc((bpr + 1) * h)) == NULL)
		return (-1);
	for (i = 0; i < h; i++) {
		raw[i * (bpr + 1)] = 0;		/* filter: none */
		if (bits == 16) {
			const uint16_t *s = (const uint16_t *)(const void *)px +
			    i * w * channels;
			for (x = 0; x < (size_t)w * channels; x++) {
				raw[i * (bpr + 1) + 1 + 2 * x] = s[x] >> 8;
				raw[i * (bpr + 1) + 2 + 2 * x] = s[x] & 0xff;
			}
		} else
			memcpy(raw + i * (bpr + 1) + 1, px + i * bpr, bpr);
	}
	zlen = compressBound((bpr + 1) * h);
	if ((z = malloc(zlen)) == NULL) {
		free(raw);
		return (-1);
	}
	error = compress2(z, &zlen, raw, (bpr + 1) * h, 6) != Z_OK;
	free(raw);
	be32(ihdr, w);
	be32(ihdr + 4, h);
	ihdr[8] = bits;
	ihdr[9] = channels == 3 ? 2 : 0;
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	if (!error)
		error = fwrite(sig, 1, 8, fp) != 8 || chunk(fp, "IHDR", ihdr, 13) ||
		    chunk(fp, "IDAT", z, (uint32_t)zlen) ||
		    chunk(fp, "IEND", NULL, 0);
	free(z);
	return (error ? -1 : 0);
}

/*
 * The autofocus' contrast (pro1x-camera's sharpness(): the sum of the
 * neighbour differences in the middle half), on the green of each 2x2 quad
 */
double
sharpness(const uint16_t *px, uint32_t w, uint32_t h)
{
	uint32_t x, y;
	double sum = 0;

	for (y = h / 4 & ~1u; y + 2 < 3 * h / 4; y += 2) {
		const uint16_t *r = px + (size_t)y * w, *b = r + 2 * (size_t)w;

		for (x = w / 4 & ~1u; x + 2 < 3 * w / 4; x += 2) {
			int g = r[x + 1] + r[x + w], gx = r[x + 3] + r[x + 2 + w],
			    gy = b[x + 1] + b[x + w];
			sum += abs(gx - g) + abs(gy - g);
		}
	}
	return (sum);
}
