/*
 * The shadow to the framebuffer for Option "Rotate" "CW" at 32 bits per
 * pixel (the F(x)tec Pro1X: a 1080 x 2160 portrait panel, X's screen
 * 2160 x 1080): shadow pixel (x, y) is framebuffer row x, column
 * h - 1 - y, h the screen's height (ScfbPointerMoved's mapping).
 *
 * The server's shadowUpdateRotatePacked walks the framebuffer's rows and
 * reads the shadow down a column: a new cache line and, with 8640 bytes a
 * row, a new page for every pixel, about 2.3 million of them for the
 * whole screen. Here the box goes in bands of 64 shadow rows; in a band,
 * blocks of 4 columns by 16 rows: four 4 x 4 transposes in NEON
 * registers, written as 64 contiguous bytes to each of the 4
 * framebuffer rows (a whole line for the write-combining buffers). The
 * box's edges that do not fill a block go pixel by pixel.
 *
 * Plain C for the PC test (freebsd/scfb/test), no X types.
 */
#ifndef SCFB_ROTCW_H
#define SCFB_ROTCW_H

#include <stddef.h>
#include <stdint.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define	ROTCW_BAND	64

/* (x1, y1)-(x2, y2), x2 and y2 exclusive; sstride and dstride in bytes */
static void
rotcw_pixels(const uint8_t *sha, ptrdiff_t sstride, uint8_t *fb,
    ptrdiff_t dstride, int h, int x1, int y1, int x2, int y2)
{
	int x, y;

	for (x = x1; x < x2; x++) {
		uint32_t *d = (uint32_t *)(fb + x * dstride) + (h - y2);
		const uint8_t *s = sha + (y2 - 1) * sstride + x * 4;

		for (y = y2 - 1; y >= y1; y--, s -= sstride)
			*d++ = *(const uint32_t *)s;
	}
}

/* Shadow columns x0..x0+3, rows y0..y0+15 */
static inline void
rotcw_block(const uint8_t *sha, ptrdiff_t sstride, uint8_t *fb,
    ptrdiff_t dstride, int h, int x0, int y0)
{
	uint32_t *d[4];
	int g, i;

	for (i = 0; i < 4; i++)
		d[i] = (uint32_t *)(fb + (x0 + i) * dstride) + (h - 1 - (y0 + 15));
	for (g = 0; g < 4; g++) {
		/* rows y0+15-4g down to y0+12-4g: framebuffer columns 4g..4g+3 */
		const uint8_t *s = sha + (y0 + 15 - 4 * g) * sstride + x0 * 4;
#if defined(__aarch64__)
		uint32x4_t r0 = vld1q_u32((const uint32_t *)s);
		uint32x4_t r1 = vld1q_u32((const uint32_t *)(s - sstride));
		uint32x4_t r2 = vld1q_u32((const uint32_t *)(s - 2 * sstride));
		uint32x4_t r3 = vld1q_u32((const uint32_t *)(s - 3 * sstride));
		uint32x4_t t0 = vtrn1q_u32(r0, r1), t1 = vtrn2q_u32(r0, r1);
		uint32x4_t t2 = vtrn1q_u32(r2, r3), t3 = vtrn2q_u32(r2, r3);

		vst1q_u32(d[0] + 4 * g, vreinterpretq_u32_u64(vtrn1q_u64(
		    vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))));
		vst1q_u32(d[1] + 4 * g, vreinterpretq_u32_u64(vtrn1q_u64(
		    vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))));
		vst1q_u32(d[2] + 4 * g, vreinterpretq_u32_u64(vtrn2q_u64(
		    vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))));
		vst1q_u32(d[3] + 4 * g, vreinterpretq_u32_u64(vtrn2q_u64(
		    vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))));
#else
		int k;

		for (k = 0; k < 4; k++)
			for (i = 0; i < 4; i++)
				d[i][4 * g + k] = ((const uint32_t *)(s - k *
				    sstride))[i];
#endif
	}
}

static void
rotcw_box(const uint8_t *sha, ptrdiff_t sstride, uint8_t *fb,
    ptrdiff_t dstride, int h, int x1, int y1, int x2, int y2)
{
	int bx2, by2, band, band2, x0, y0;

	if (x1 >= x2 || y1 >= y2)
		return;
	bx2 = x1 + ((x2 - x1) & ~3);
	by2 = y1 + ((y2 - y1) & ~15);
	for (band = y1; band < by2; band = band2) {
		band2 = band + ROTCW_BAND < by2 ? band + ROTCW_BAND : by2;
		for (x0 = x1; x0 < bx2; x0 += 4)
			for (y0 = band; y0 < band2; y0 += 16)
				rotcw_block(sha, sstride, fb, dstride, h, x0, y0);
	}
	/* the right edge's columns, then the bottom edge's rows */
	rotcw_pixels(sha, sstride, fb, dstride, h, bx2, y1, x2, y2);
	rotcw_pixels(sha, sstride, fb, dstride, h, x1, by2, bx2, y2);
}

#endif /* SCFB_ROTCW_H */
