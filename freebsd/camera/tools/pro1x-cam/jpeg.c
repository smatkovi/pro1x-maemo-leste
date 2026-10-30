/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A baseline JPEG (JFIF) encoder, so that pro1x-cam needs nothing outside
 * FreeBSD's base system: YCbCr 4:2:0, the example quantisation tables of
 * ITU-T T.81 Annex K.1 scaled by quality as the IJG library does, the
 * Huffman tables of Annex K.3, a separable floating point DCT.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cam.h"

static const uint8_t zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Annex K.1, natural order */
static const uint8_t q_lum[64] = {
	16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t q_chr[64] = {
	17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* Annex K.3: code lengths 1-16, then the values */
static const uint8_t dc_lum_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1 };
static const uint8_t dc_chr_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4,
	0, 0, 1, 0x7d };
static const uint8_t ac_lum_vals[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
	0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
	0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
	0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
	0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
	0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
	0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
	0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t ac_chr_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4,
	0, 1, 2, 0x77 };
static const uint8_t ac_chr_vals[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
	0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
	0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
	0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
	0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
	0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
	0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

struct huff {
	uint16_t	code[256];
	uint8_t		len[256];
};

struct jw {
	FILE		*fp;
	uint32_t	acc;
	int		nbits;
	int		err;
	struct huff	dc[2], ac[2];
	float		q[2][64];	/* 1 / (8 * table), natural order */
	uint8_t		qt[2][64];
	float		cosv[8][8];
};

/* Annex C: codes from the lengths */
static void
huff_build(struct huff *h, const uint8_t *bits, const uint8_t *vals)
{
	uint16_t code = 0;
	int i, j, k = 0;

	memset(h, 0, sizeof(*h));
	for (i = 0; i < 16; i++) {
		for (j = 0; j < bits[i]; j++) {
			h->code[vals[k]] = code++;
			h->len[vals[k]] = i + 1;
			k++;
		}
		code <<= 1;
	}
}

static void
put_bits(struct jw *j, uint32_t v, int n)
{
	uint8_t b;

	j->acc = j->acc << n | (v & ((1u << n) - 1));
	j->nbits += n;
	while (j->nbits >= 8) {
		b = (uint8_t)(j->acc >> (j->nbits - 8));
		if (fputc(b, j->fp) == EOF)
			j->err = 1;
		if (b == 0xff && fputc(0, j->fp) == EOF)	/* stuffing */
			j->err = 1;
		j->nbits -= 8;
	}
	j->acc &= (1u << j->nbits) - 1;
}

static void
put16(FILE *fp, int v)
{
	fputc(v >> 8, fp);
	fputc(v & 0xff, fp);
}

static void
dht(FILE *fp, int class, int id, const uint8_t *bits, const uint8_t *vals)
{
	int i, n = 0;

	for (i = 0; i < 16; i++)
		n += bits[i];
	put16(fp, 0xffc4);
	put16(fp, 2 + 1 + 16 + n);
	fputc(class << 4 | id, fp);
	fwrite(bits, 1, 16, fp);
	fwrite(vals, 1, n, fp);
}

static int
category(int v)
{
	int n = 0;

	if (v < 0)
		v = -v;
	while (v) {
		n++;
		v >>= 1;
	}
	return (n);
}

static void
put_value(struct jw *j, int v, int n)
{
	if (n)
		put_bits(j, v < 0 ? (uint32_t)(v - 1) : (uint32_t)v, n);
}

/* one 8x8 block of samples - 128: DCT, quantise, code; dc: predictor */
static void
block(struct jw *j, const float *in, int t, int *dc)
{
	float tmp[64], co[64];
	int q[64], x, y, u, v, k, run, d, n;

	for (y = 0; y < 8; y++)
		for (u = 0; u < 8; u++) {
			float s = 0;
			for (x = 0; x < 8; x++)
				s += in[y * 8 + x] * j->cosv[u][x];
			tmp[y * 8 + u] = s;
		}
	for (u = 0; u < 8; u++)
		for (v = 0; v < 8; v++) {
			float s = 0;
			for (y = 0; y < 8; y++)
				s += tmp[y * 8 + u] * j->cosv[v][y];
			co[v * 8 + u] = s;
		}
	for (k = 0; k < 64; k++)
		q[k] = (int)lrintf(co[zigzag[k]] * j->q[t][zigzag[k]]);
	d = q[0] - *dc;
	*dc = q[0];
	n = category(d);
	put_bits(j, j->dc[t].code[n], j->dc[t].len[n]);
	put_value(j, d, n);
	for (k = 1, run = 0; k < 64; k++) {
		if (q[k] == 0) {
			run++;
			continue;
		}
		while (run > 15) {
			put_bits(j, j->ac[t].code[0xf0], j->ac[t].len[0xf0]);
			run -= 16;
		}
		n = category(q[k]);
		put_bits(j, j->ac[t].code[run << 4 | n], j->ac[t].len[run << 4 | n]);
		put_value(j, q[k], n);
		run = 0;
	}
	if (run)
		put_bits(j, j->ac[t].code[0], j->ac[t].len[0]);
}

static float
pix(const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t x, uint32_t y, int c)
{
	if (x >= w)
		x = w - 1;
	if (y >= h)
		y = h - 1;
	return (rgb[((size_t)y * w + x) * 3 + c]);
}

int
jpeg_write(FILE *fp, const uint8_t *rgb, uint32_t w, uint32_t h, int quality)
{
	static const uint8_t jfif[14] = { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1,
	    0, 1, 0, 0 };
	struct jw *j;
	float Y[4][64], Cb[64], Cr[64];
	uint32_t mx, my, x, y, b;
	int dc[3] = { 0, 0, 0 }, scale, t, k, i, u;

	if ((j = calloc(1, sizeof(*j))) == NULL)
		return (-1);
	j->fp = fp;
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
	for (k = 0; k < 64; k++) {
		t = (q_lum[k] * scale + 50) / 100;
		j->qt[0][k] = t < 1 ? 1 : t > 255 ? 255 : t;
		t = (q_chr[k] * scale + 50) / 100;
		j->qt[1][k] = t < 1 ? 1 : t > 255 ? 255 : t;
	}
	/* the DCT's scale: C(u)/2 per dimension, folded into the table */
	for (u = 0; u < 8; u++)
		for (x = 0; x < 8; x++)
			j->cosv[u][x] = (float)((u ? 1 : M_SQRT1_2) / 2 *
			    cos((2 * x + 1) * u * M_PI / 16));
	for (k = 0; k < 64; k++) {
		j->q[0][k] = 1.0f / j->qt[0][k];
		j->q[1][k] = 1.0f / j->qt[1][k];
	}
	huff_build(&j->dc[0], dc_lum_bits, dc_vals);
	huff_build(&j->dc[1], dc_chr_bits, dc_vals);
	huff_build(&j->ac[0], ac_lum_bits, ac_lum_vals);
	huff_build(&j->ac[1], ac_chr_bits, ac_chr_vals);

	put16(fp, 0xffd8);
	put16(fp, 0xffe0);
	put16(fp, 16);
	fwrite(jfif, 1, sizeof(jfif), fp);
	for (t = 0; t < 2; t++) {
		put16(fp, 0xffdb);
		put16(fp, 67);
		fputc(t, fp);
		for (k = 0; k < 64; k++)
			fputc(j->qt[t][zigzag[k]], fp);
	}
	put16(fp, 0xffc0);
	put16(fp, 17);
	fputc(8, fp);
	put16(fp, h);
	put16(fp, w);
	fputc(3, fp);
	fputc(1, fp); fputc(0x22, fp); fputc(0, fp);	/* Y 2x2, table 0 */
	fputc(2, fp); fputc(0x11, fp); fputc(1, fp);	/* Cb */
	fputc(3, fp); fputc(0x11, fp); fputc(1, fp);	/* Cr */
	dht(fp, 0, 0, dc_lum_bits, dc_vals);
	dht(fp, 1, 0, ac_lum_bits, ac_lum_vals);
	dht(fp, 0, 1, dc_chr_bits, dc_vals);
	dht(fp, 1, 1, ac_chr_bits, ac_chr_vals);
	put16(fp, 0xffda);
	put16(fp, 12);
	fputc(3, fp);
	fputc(1, fp); fputc(0x00, fp);
	fputc(2, fp); fputc(0x11, fp);
	fputc(3, fp); fputc(0x11, fp);
	fputc(0, fp); fputc(63, fp); fputc(0, fp);

	for (my = 0; my < (h + 15) / 16; my++) {
		for (mx = 0; mx < (w + 15) / 16; mx++) {
			memset(Cb, 0, sizeof(Cb));
			memset(Cr, 0, sizeof(Cr));
			for (y = 0; y < 16; y++)
				for (x = 0; x < 16; x++) {
					uint32_t px = mx * 16 + x, py = my * 16 + y;
					float r = pix(rgb, w, h, px, py, 0),
					    g = pix(rgb, w, h, px, py, 1),
					    bb = pix(rgb, w, h, px, py, 2);
					b = (y / 8) * 2 + x / 8;
					Y[b][(y % 8) * 8 + x % 8] = 0.299f * r +
					    0.587f * g + 0.114f * bb - 128;
					i = (y / 2) * 8 + x / 2;
					Cb[i] += (-0.168736f * r - 0.331264f * g +
					    0.5f * bb) / 4;
					Cr[i] += (0.5f * r - 0.418688f * g -
					    0.081312f * bb) / 4;
				}
			for (b = 0; b < 4; b++)
				block(j, Y[b], 0, &dc[0]);
			block(j, Cb, 1, &dc[1]);
			block(j, Cr, 1, &dc[2]);
		}
	}
	if (j->nbits)
		put_bits(j, 0x7f, 8 - j->nbits);	/* pad with ones */
	put16(fp, 0xffd9);
	k = j->err || ferror(fp);
	free(j);
	return (k ? -1 : 0);
}
