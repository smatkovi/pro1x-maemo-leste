/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The interface between snd_pro1x.ko (sound(4): /dev/dsp, /dev/mixer) and
 * pro1x-audiod, which moves the PCM to and from the ADSP
 * (freebsd/audio/README.md, "sound(4)"):
 *
 * - each sound(4) channel's hardware buffer is physically contiguous,
 *   below 4 GiB and uncached; the ADSP reads (playback) or writes
 *   (capture) it in blocks, by physical address, through the ASM data
 *   commands the daemon sends;
 * - /dev/pro1x_pcm (one opener: the daemon) carries events from the
 *   driver (read(2): whole struct pro1x_pcm_event records; poll(2)):
 *   a channel started or stopped, a mixer level or the recording source
 *   changed;
 * - PRO1X_PCM_ADVANCE tells the driver that the ADSP is done with
 *   `blocks` more blocks of a channel (WRITE_DONE, READ_DONE): it moves
 *   the channel's pointer and runs sound(4)'s interrupt (chn_intr) once
 *   per block. The answer says how far sound(4) has filled the buffer
 *   (playback: `ready` blocks from the block the ADSP is at), which is
 *   how many the daemon may hand to the ADSP;
 * - without the daemon (the device not open, or PRO1X_PCM_FAIL) a clock
 *   in the driver consumes (or fills with silence) one block per block
 *   time, so that writers never hang.
 *
 * The block size is a multiple of 480 frames (10 ms at 48 kHz, as Linux's
 * q6asm-dai constrains its periods); the rate is 48 kHz, 16 bit.
 */

#ifndef _PRO1X_PCM_H_
#define	_PRO1X_PCM_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	PRO1X_PCM_DEV		"/dev/pro1x_pcm"
#define	PRO1X_PCM_PLAY		0
#define	PRO1X_PCM_REC		1
#define	PRO1X_PCM_NCHAN		2
#define	PRO1X_PCM_BUFMAX	(64 * 1024)	/* per channel, mmap offset */
#define	PRO1X_PCM_FRAMES_STEP	480

/* event types */
#define	PRO1X_PCM_EV_HELLO	1	/* first after open: the buffers */
#define	PRO1X_PCM_EV_START	2
#define	PRO1X_PCM_EV_STOP	3
#define	PRO1X_PCM_EV_MIXER	4
#define	PRO1X_PCM_EV_RECSRC	5

struct pro1x_pcm_event {
	uint32_t	type;
	uint32_t	chan;		/* PRO1X_PCM_PLAY, PRO1X_PCM_REC */
	uint32_t	gen;		/* the start's number (per channel) */
	uint32_t	rate;		/* Hz */
	uint32_t	channels;
	uint32_t	bits;
	uint32_t	blksz;		/* bytes */
	uint32_t	blkcnt;
	uint32_t	ready;		/* START, playback: blocks filled */
	uint32_t	dev;		/* MIXER: SOUND_MIXER_* */
	uint32_t	left, right;	/* MIXER: 0-100 */
	uint32_t	recsrc;		/* RECSRC: SOUND_MASK_* */
	uint32_t	bufsize;	/* the allocation, a page multiple */
	uint64_t	paddr;		/* the buffer's physical address */
	uint64_t	dspaddr;	/* for the ADSP: paddr | sid << 32 */
};

struct pro1x_pcm_advance {
	uint32_t	chan;		/* in */
	uint32_t	gen;		/* in: the START's */
	uint32_t	blocks;		/* in: blocks the ADSP is done with */
	int32_t		error;		/* in (FAIL): why the daemon gives up */
	uint32_t	hw;		/* out: blocks done since the start */
	uint32_t	ready;		/* out, playback: blocks filled from hw on */
	uint32_t	running;	/* out: 0 once the channel stopped */
	uint32_t	pad;
};

struct pro1x_pcm_status {
	uint32_t	chan;		/* in */
	uint32_t	running, gen, hw, ready, blksz, blkcnt, by_daemon;
	uint32_t	rate, channels, bits, xruns;
	uint64_t	paddr, dspaddr;
};

#define	PRO1X_PCM_ADVANCE	_IOWR('P', 1, struct pro1x_pcm_advance)
#define	PRO1X_PCM_FAIL		_IOWR('P', 2, struct pro1x_pcm_advance)
#define	PRO1X_PCM_STATUS	_IOWR('P', 3, struct pro1x_pcm_status)

#endif /* _PRO1X_PCM_H_ */
