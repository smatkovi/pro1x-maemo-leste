/*
 * scfb_rotcw.h against the mapping it implements, on random boxes of a
 * 2160 x 1080 screen, then the time of a whole-screen update: rotcw_box
 * and the server's way (framebuffer rows in order, the shadow read down a
 * column, as miext/shadow's shrotpack.h does for a 270 rotation).
 *
 *   cc -O2 -o rotcw-test rotcw-test.c && ./rotcw-test [runs]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../scfb_rotcw.h"

#define	W	2160
#define	H	1080

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

/* The server's order: each framebuffer row, the shadow down a column */
static void
column_walk(const uint32_t *sha, uint32_t *fb, int x1, int y1, int x2, int y2)
{
	int x, y;

	for (x = x1; x < x2; x++) {
		uint32_t *d = fb + x * H + (H - y2);
		const uint32_t *s = sha + (y2 - 1) * W + x;

		for (y = y2 - 1; y >= y1; y--, s -= W)
			*d++ = *s;
	}
}

int
main(int argc, char **argv)
{
	uint32_t *sha, *fb, *ref;
	int i, n, runs, bad = 0;
	double t, tb, tc;

	runs = argc > 1 ? atoi(argv[1]) : 20;
	sha = malloc(W * H * 4);
	fb = malloc(W * H * 4);
	ref = malloc(W * H * 4);
	for (i = 0; i < W * H; i++)
		sha[i] = (uint32_t)i * 2654435761u;
	srand(1);
	for (n = 0; n < 2000; n++) {
		int x1 = rand() % W, y1 = rand() % H;
		int x2 = x1 + rand() % (W - x1 + 1), y2 = y1 + rand() % (H - y1 + 1);
		int x, y;

		if (n == 0) { x1 = 0; y1 = 0; x2 = W; y2 = H; }
		memset(fb, 0x55, W * H * 4);
		memset(ref, 0x55, W * H * 4);
		rotcw_box((const uint8_t *)sha, W * 4, (uint8_t *)fb, H * 4, H,
		    x1, y1, x2, y2);
		for (x = x1; x < x2; x++)
			for (y = y1; y < y2; y++)
				ref[x * H + (H - 1 - y)] = sha[y * W + x];
		if (memcmp(fb, ref, W * H * 4) != 0) {
			if (bad++ < 5)
				printf("box %d,%d-%d,%d differs\n", x1, y1, x2, y2);
		}
	}
	printf("%d of 2000 boxes wrong\n", bad);
	t = now();
	for (i = 0; i < runs; i++)
		column_walk(sha, fb, 0, 0, W, H);
	tc = (now() - t) / runs;
	t = now();
	for (i = 0; i < runs; i++)
		rotcw_box((const uint8_t *)sha, W * 4, (uint8_t *)fb, H * 4, H,
		    0, 0, W, H);
	tb = (now() - t) / runs;
	printf("whole screen: column walk %.2f ms, blocks %.2f ms (%.1fx)\n",
	    tc * 1e3, tb * 1e3, tc / tb);
	return (bad != 0);
}
