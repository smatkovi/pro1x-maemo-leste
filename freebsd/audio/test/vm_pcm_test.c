/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * vm_pcm_test: snd_pro1x.ko's daemon side in the FreeBSD VM (vm-pcm.sh),
 * with an ADSP simulated here: on a channel's START it "plays" the blocks
 * sound(4) has filled (at most `ready`, as pro1x-audiod hands them out),
 * one per block time, reading each through the driver's mmap of the
 * buffer (the same physical pages the ADSP would read), and tells the
 * driver with PRO1X_PCM_ADVANCE; for recording it writes a ramp into each
 * block before the ADVANCE. It counts what it saw:
 *
 *   vm_pcm_test SECONDS [quit_after_blocks]
 *
 * "play: N blocks, S with sound, peak P; rec: M blocks; ..." at the end.
 * quit_after_blocks: exit (closing /dev/pro1x_pcm) after that many
 * playback blocks, to test that the driver's clock takes over.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../sys/arm64/qualcomm/audio/pro1x_pcm.h"

struct ch {
	bool run;
	uint32_t gen, blksz, blkcnt, ready, hw, submitted, channels;
	double next, period;
	unsigned blocks, loud, maxinflight, events, stale;
	int peak;
};

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec / 1e9);
}

int
main(int argc, char **argv)
{
	struct pro1x_pcm_event ev[16];
	struct pro1x_pcm_advance a;
	struct ch c[2];
	struct pollfd p;
	double end, t;
	uint8_t *buf;
	int fd, n, i, j, k, quit = argc > 2 ? atoi(argv[2]) : 0;
	unsigned hellos = 0, mixers = 0, recsrcs = 0;

	end = now() + (argc > 1 ? atof(argv[1]) : 5);
	if ((fd = open(PRO1X_PCM_DEV, O_RDWR | O_NONBLOCK)) < 0) {
		perror(PRO1X_PCM_DEV);
		return (1);
	}
	if (open(PRO1X_PCM_DEV, O_RDWR) >= 0) {
		printf("FAIL: a second open succeeded\n");
		return (1);
	}
	buf = mmap(NULL, 2 * PRO1X_PCM_BUFMAX, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return (1);
	}
	memset(c, 0, sizeof(c));
	while ((t = now()) < end) {
		p.fd = fd;
		p.events = POLLIN;
		poll(&p, 1, 2);
		while ((n = read(fd, ev, sizeof(ev))) > 0) {
			for (i = 0; i < n / (int)sizeof(ev[0]); i++) {
				struct pro1x_pcm_event *e = &ev[i];
				struct ch *x;

				switch (e->type) {
				case PRO1X_PCM_EV_HELLO:
					hellos++;
					printf("hello: %s buffer at %#jx, ADSP "
					    "%#jx, %u bytes\n", e->chan ? "rec" :
					    "play", (uintmax_t)e->paddr,
					    (uintmax_t)e->dspaddr, e->bufsize);
					break;
				case PRO1X_PCM_EV_MIXER:
					mixers++;
					break;
				case PRO1X_PCM_EV_RECSRC:
					recsrcs++;
					break;
				case PRO1X_PCM_EV_START:
					x = &c[e->chan];
					x->run = true;
					x->gen = e->gen;
					x->blksz = e->blksz;
					x->blkcnt = e->blkcnt;
					x->channels = e->channels;
					x->ready = e->chan ? e->blkcnt : e->ready;
					x->hw = x->submitted = 0;
					x->period = (double)e->blksz /
					    (48000.0 * 2 * e->channels);
					x->next = t + x->period;
					x->events++;
					printf("start %s gen %u: %u x %u bytes, %u "
					    "ch, ready %u, %.1f ms blocks\n",
					    e->chan ? "rec" : "play", e->gen,
					    e->blkcnt, e->blksz, e->channels,
					    e->ready, x->period * 1000);
					break;
				case PRO1X_PCM_EV_STOP:
					x = &c[e->chan];
					if (x->gen == e->gen)
						x->run = false;
					printf("stop %s gen %u after %u blocks\n",
					    e->chan ? "rec" : "play", e->gen,
					    x->blocks);
					break;
				}
			}
		}
		for (k = 0; k < 2; k++) {
			struct ch *x = &c[k];
			uint32_t idx, inflight;
			int16_t *s;

			if (!x->run || t < x->next)
				continue;
			x->next += x->period;
			/* what pro1x-audiod would have in flight */
			inflight = (x->ready < x->blkcnt ? x->ready : x->blkcnt);
			if (inflight > x->maxinflight)
				x->maxinflight = inflight;
			idx = x->hw % x->blkcnt;
			s = (int16_t *)(buf + k * PRO1X_PCM_BUFMAX + idx * x->blksz);
			if (k == 0) {
				int pk = 0;

				for (j = 0; j < (int)(x->blksz / 2); j++)
					if (abs(s[j]) > pk)
						pk = abs(s[j]);
				if (pk > 1000)
					x->loud++;
				if (pk > x->peak)
					x->peak = pk;
			} else
				for (j = 0; j < (int)(x->blksz / 2); j++)
					s[j] = (int16_t)(j * 7 - 8000);
			memset(&a, 0, sizeof(a));
			a.chan = (uint32_t)k;
			a.gen = x->gen;
			a.blocks = 1;
			if (ioctl(fd, PRO1X_PCM_ADVANCE, &a) != 0) {
				perror("ADVANCE");
				return (1);
			}
			if (!a.running) {
				x->stale++;
				x->run = false;
				continue;
			}
			x->hw = a.hw;
			x->ready = a.ready;
			x->blocks++;
			if (k == 0 && quit && (int)x->blocks >= quit) {
				printf("quitting after %u blocks: the driver's "
				    "clock goes on\n", x->blocks);
				goto out;
			}
		}
	}
out:
	printf("events: %u hello, %u mixer, %u recsrc\n", hellos, mixers,
	    recsrcs);
	printf("play: %u starts, %u blocks, %u with sound, peak %d, ready at "
	    "most %u\n", c[0].events, c[0].blocks, c[0].loud, c[0].peak,
	    c[0].maxinflight);
	printf("rec: %u starts, %u blocks\n", c[1].events, c[1].blocks);
	return (0);
}
