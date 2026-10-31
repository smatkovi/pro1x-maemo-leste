/*
 * PVRTC1 decoder, see pvrtc-decode.h for the format. Written for this
 * repository from the public format description (no third-party code).
 */
#include <stdlib.h>
#include <string.h>

#include "pvrtc-decode.h"

static int pot(int v)
{
	return v > 0 && (v & (v - 1)) == 0;
}

size_t pvrtc_size(int w, int h, int bpp)
{
	int pw = bpp == 2 ? (w < 16 ? 16 : w) : (w < 8 ? 8 : w);
	int ph = h < 8 ? 8 : h;
	return (size_t)pw * ph * bpp / 8;
}

/* Index of block (x, y) in Morton order: the bits of both coordinates
 * interleaved up to the smaller dimension, y in the lower bit of each pair;
 * the remaining high bits of the larger dimension's coordinate on top. */
static unsigned morton(unsigned x, unsigned y, unsigned nbx, unsigned nby)
{
	unsigned min = nbx < nby ? nbx : nby, r = 0, sh = 0;
	for (unsigned m = 1; m < min; m <<= 1, sh += 2) {
		if (y & m)
			r |= 1u << sh;
		if (x & m)
			r |= 2u << sh;
	}
	return r | (((nbx > nby ? x : y) >> (sh / 2)) << sh);
}

/* Colours as 5-bit RGB and 4-bit alpha, in c[0..3] = r, g, b, a. */
static void colour_a(unsigned c, int *o)
{
	if (c & 0x8000) {		/* RGB 5:5:4 */
		unsigned b = (c >> 1) & 15;
		o[0] = (c >> 10) & 31;
		o[1] = (c >> 5) & 31;
		o[2] = (b << 1) | (b >> 3);
		o[3] = 15;
	} else {			/* ARGB 3:4:4:3 */
		unsigned r = (c >> 8) & 15, g = (c >> 4) & 15, b = (c >> 1) & 7;
		o[0] = (r << 1) | (r >> 3);
		o[1] = (g << 1) | (g >> 3);
		o[2] = (b << 2) | (b >> 1);
		o[3] = ((c >> 12) & 7) << 1;
	}
}

static void colour_b(unsigned c, int *o)
{
	if (c & 0x8000) {		/* RGB 5:5:5 */
		o[0] = (c >> 10) & 31;
		o[1] = (c >> 5) & 31;
		o[2] = c & 31;
		o[3] = 15;
	} else {			/* ARGB 3:4:4:4 */
		unsigned r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
		o[0] = (r << 1) | (r >> 3);
		o[1] = (g << 1) | (g >> 3);
		o[2] = (b << 1) | (b >> 3);
		o[3] = ((c >> 12) & 7) << 1;
	}
}

static const unsigned char w_std[4] = { 0, 3, 5, 8 };
static const unsigned char w_pt[4] = { 0, 4, 4, 8 };

struct blk {
	unsigned mod;		/* modulation, 2bpp mode 1 already adjusted */
	unsigned char mode;	/* 0, or for 2bpp mode 1: 1 HV, 2 H, 3 V */
	int a[4], b[4];
};

/* Weight (0..8) stored for pixel (lx, ly) of a 2bpp block; for mode 1 only
 * meaningful where lx + ly is even. */
static int weight2_stored(const struct blk *b, int lx, int ly)
{
	if (!b->mode)
		return (b->mod >> (ly * 8 + lx)) & 1 ? 8 : 0;
	return w_std[(b->mod >> (2 * (ly * 4 + (lx >> 1)))) & 3];
}

static void decode_padded(const uint8_t *src, int w, int h, int bpp, int opaque, uint8_t *dst)
{
	const int bw = bpp == 2 ? 8 : 4, bh = 4;
	const int lbw = bpp == 2 ? 3 : 2, lbh = 2;
	const int nbx = w >> lbw, nby = h >> lbh;
	/* scale of an interpolated colour: bw*bh (bilinear) * 8 (modulation) */
	const unsigned long long mul_rgb = bpp == 2 ? (255ull << 24) / (31 * 32 * 8) : (255ull << 24) / (31 * 16 * 8);
	const unsigned long long mul_a = bpp == 2 ? (255ull << 24) / (15 * 32 * 8) : (255ull << 24) / (15 * 16 * 8);
	struct blk *bl = malloc(sizeof(*bl) * nbx * nby);
	if (!bl) {
		memset(dst, 0, (size_t)w * h * 4);
		return;
	}

	for (int by = 0; by < nby; by++) {
		for (int bx = 0; bx < nbx; bx++) {
			const uint8_t *p = src + 8 * (size_t)morton(bx, by, nbx, nby);
			unsigned mod = p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
			unsigned col = p[4] | p[5] << 8 | p[6] << 16 | (unsigned)p[7] << 24;
			struct blk *b = &bl[by * nbx + bx];
			colour_a(col & 0xffff, b->a);
			colour_b(col >> 16, b->b);
			b->mode = col & 1;
			if (bpp == 2 && b->mode) {
				if (mod & 1) {
					b->mode = mod & (1u << 20) ? 3 : 2;
					mod = (mod & ~(1u << 20)) | ((mod >> 1) & (1u << 20));
				}
				mod = (mod & ~1u) | ((mod >> 1) & 1u);
			}
			b->mod = mod;
		}
	}

	for (int y = 0; y < h; y++) {
		int fy = y - bh / 2;
		int y0 = (fy >> lbh) & (nby - 1), y1 = ((fy >> lbh) + 1) & (nby - 1);
		int wy = fy & (bh - 1);
		for (int x = 0; x < w; x++) {
			int fx = x - bw / 2;
			int x0 = (fx >> lbw) & (nbx - 1), x1 = ((fx >> lbw) + 1) & (nbx - 1);
			int wx = fx & (bw - 1);
			const struct blk *p = &bl[y0 * nbx + x0], *q = &bl[y0 * nbx + x1];
			const struct blk *r = &bl[y1 * nbx + x0], *s = &bl[y1 * nbx + x1];
			int k00 = (bw - wx) * (bh - wy), k10 = wx * (bh - wy);
			int k01 = (bw - wx) * wy, k11 = wx * wy;

			/* modulation of the pixel's own block */
			const struct blk *own = &bl[(y >> lbh) * nbx + (x >> lbw)];
			int lx = x & (bw - 1), ly = y & (bh - 1), m, pt = 0;
			if (bpp == 4) {
				int v = (own->mod >> (2 * (ly * 4 + lx))) & 3;
				if (own->mode) {
					m = w_pt[v];
					pt = v == 2;
				} else
					m = w_std[v];
			} else if (!own->mode || ((lx ^ ly) & 1) == 0) {
				m = weight2_stored(own, lx, ly);
			} else {
				/* neighbours, possibly in the next blocks (wrapping) */
				int xl = (x - 1) & (w - 1), xr = (x + 1) & (w - 1);
				int yu = (y - 1) & (h - 1), yd = (y + 1) & (h - 1);
#define N(px, py) weight2_stored(&bl[((py) >> lbh) * nbx + ((px) >> lbw)], (px) & (bw - 1), (py) & (bh - 1))
				if (own->mode == 1)
					m = (N(xl, y) + N(xr, y) + N(x, yu) + N(x, yd) + 2) / 4;
				else if (own->mode == 2)
					m = (N(xl, y) + N(xr, y) + 1) / 2;
				else
					m = (N(x, yu) + N(x, yd) + 1) / 2;
#undef N
			}

			uint8_t *o = dst + 4 * ((size_t)y * w + x);
			for (int c = 0; c < 4; c++) {
				unsigned ca = p->a[c] * k00 + q->a[c] * k10 + r->a[c] * k01 + s->a[c] * k11;
				unsigned cb = p->b[c] * k00 + q->b[c] * k10 + r->b[c] * k01 + s->b[c] * k11;
				unsigned f = ca * (8 - m) + cb * m;
				unsigned v = (unsigned)((f * (c == 3 ? mul_a : mul_rgb) + (1u << 23)) >> 24);
				o[c] = v > 255 ? 255 : v;
			}
			if (opaque)
				o[3] = 255;
			else if (pt)
				o[3] = 0;
		}
	}
	free(bl);
}

int pvrtc_decode(const uint8_t *src, int w, int h, int bpp, int opaque, uint8_t *dst)
{
	if (!pot(w) || !pot(h) || (bpp != 2 && bpp != 4))
		return -1;
	int pw = bpp == 2 ? (w < 16 ? 16 : w) : (w < 8 ? 8 : w);
	int ph = h < 8 ? 8 : h;
	if (pw == w && ph == h) {
		decode_padded(src, w, h, bpp, opaque, dst);
		return 0;
	}
	uint8_t *tmp = malloc((size_t)pw * ph * 4);
	if (!tmp)
		return -1;
	decode_padded(src, pw, ph, bpp, opaque, tmp);
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w * 4, tmp + (size_t)y * pw * 4, (size_t)w * 4);
	free(tmp);
	return 0;
}
