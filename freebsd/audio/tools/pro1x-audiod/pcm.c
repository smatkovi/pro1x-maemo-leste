/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * snd_pro1x.ko's two channels as ASM sessions (pro1x_pcm.h): a START
 * brings the channel's back end up (backend.c: the AFE port, the codec),
 * then the session as Linux's q6asm_dai_prepare and _trigger send it
 * (q6.c), and hands the ADSP the blocks sound(4) has filled; every
 * WRITE_DONE (READ_DONE) tells the kernel one block is done
 * (PRO1X_PCM_ADVANCE), which runs sound(4)'s interrupt, and the answer
 * says how many blocks may be in flight now. A STOP closes the session;
 * the back end goes down later (backend_put: the idle delay).
 *
 * Playback hands out only blocks sound(4) has filled (from the block the
 * ADSP is at, `ready`), so the ADSP never plays what is being written;
 * capture keeps every block of the ring queued for reading.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audiod.h"

static const char *
chname(int chan)
{
	return (chan == PRO1X_PCM_PLAY ? "playback" : "capture");
}

static void
fail_kernel(struct audiod *d, struct stream *st, int r)
{
	struct pro1x_pcm_advance a;

	memset(&a, 0, sizeof(a));
	a.chan = (uint32_t)st->chan;
	a.gen = st->gen;
	a.error = r;
	if (d->pcm->fail != NULL)
		d->pcm->fail(d->pcm->ctx, &a);
}

static int
submit(struct audiod *d, struct stream *st, uint32_t idx)
{
	uint8_t pkt[64];
	uint64_t addr = st->dspaddr + (uint64_t)idx * st->blksz;
	size_t len;

	if (st->chan == PRO1X_PCM_PLAY)
		len = asm_pkt_write(pkt, st->session, addr, st->maphandle,
		    st->blksz, idx);
	else
		len = asm_pkt_read(pkt, st->session, addr, st->maphandle,
		    st->blksz, idx);
	return (apr_send(&d->q, pkt, len));
}

/* hand the ADSP what may be in flight: playback up to hw + ready */
static int
refill(struct audiod *d, struct stream *st)
{
	uint32_t limit = st->chan == PRO1X_PCM_PLAY ? st->hw + st->ready :
	    st->hw + st->blkcnt;
	int r;

	if (st->chan == PRO1X_PCM_PLAY && st->ready > st->blkcnt)
		limit = st->hw + st->blkcnt;
	/*
	 * Nothing in flight and nothing ready would stall the stream for
	 * good (no more done events): one block anyway. sound(4) keeps its
	 * buffer filled (silence on underrun), so this is not expected.
	 */
	if (st->submitted == st->hw && limit == st->hw) {
		limit = st->hw + 1;
		if (st->errors++ < 5)
			fma_log("pcm: %s: no block ready, one sent anyway",
			    chname(st->chan));
	}
	while ((int32_t)(limit - st->submitted) > 0 &&
	    st->submitted - st->hw < st->blkcnt) {
		if ((r = submit(d, st, st->submitted % st->blkcnt)) != 0)
			return (r);
		st->submitted++;
	}
	return (0);
}

static void
close_session(struct audiod *d, struct stream *st, int step)
{
	uint8_t pkt[64];
	size_t len;
	int path = st->chan == PRO1X_PCM_PLAY ? ADM_PATH_PLAYBACK :
	    ADM_PATH_LIVE_REC;

	(void)path;
	/*
	 * step: how far start() got. Linux's order (q6asm_dai_close, and
	 * q6asm_dai_prepare's error path): CLOSE, unmap, then the routing's
	 * COPP (q6routing_stream_close)
	 */
	if (step >= 3) {
		len = asm_pkt_cmd(pkt, st->session, ASM_STREAM_CMD_CLOSE);
		asm_cmd(&d->q, pkt, len, "close");
	}
	if (step >= 2)
		asm_unmap(&d->q, st->session, st->chan, st->maphandle);
	if (step >= 4)
		adm_close(&d->q, st->afe_port, st->afe_idx, st->copp);
	st->maphandle = 0;
	st->copp = INVALID_COPP_ID;
	if (step >= 1)
		backend_put(d, st->be);
}

static int
start(struct audiod *d, struct stream *st, uint32_t ready)
{
	uint8_t pkt[96];
	size_t len;
	int be, r, step = 0, path;
	struct backend *b;

	/* the microphone as the jack decides it, kept for the stop */
	be = st->chan == PRO1X_PCM_PLAY ? BE_RX :
	    strcmp(eff_recroute(d), "amic") == 0 ? BE_TX : BE_VA;
	st->be = be;
	b = &d->be[be];
	path = st->chan == PRO1X_PCM_PLAY ? ADM_PATH_PLAYBACK :
	    ADM_PATH_LIVE_REC;
	st->afe_port = b->afe_port;
	st->afe_idx = b->afe_idx;
	st->copp = INVALID_COPP_ID;
	st->maphandle = 0;
	st->hw = st->submitted = 0;
	st->ready = ready;
	st->running = false;
	if ((r = backend_get(d, be)) != 0)
		goto fail;
	step = 1;
	if ((r = asm_map(&d->q, st->session, st->chan, st->dspaddr,
	    st->blksz * st->blkcnt, &st->maphandle)) != 0)
		goto fail;
	step = 2;
	if (st->chan == PRO1X_PCM_PLAY)
		len = asm_pkt_open_write(pkt, st->session, st->bits);
	else
		len = asm_pkt_open_read(pkt, st->session, st->bits);
	if ((r = asm_cmd(&d->q, pkt, len, "open")) != 0)
		goto fail;
	step = 3;
	/* q6routing_stream_open: the COPP at the back end's format */
	if ((r = adm_open(&d->q, b->afe_port, b->afe_idx, path, 48000,
	    b->channels, 16, &st->copp)) != 0)
		goto fail;
	step = 4;
	if ((r = adm_map(&d->q, path, st->session, st->copp)) != 0)
		goto fail;
	if (st->chan == PRO1X_PCM_PLAY) {
		len = asm_pkt_media_fmt(pkt, st->session, st->rate,
		    st->channels, st->bits);
		if ((r = asm_cmd(&d->q, pkt, len, "media format")) != 0)
			goto fail;
	} else {
		len = asm_pkt_enc_cfg(pkt, st->session, st->rate, st->channels,
		    st->bits);
		if ((r = asm_cmd(&d->q, pkt, len, "encoder config")) != 0)
			goto fail;
	}
	/* the blocks first (Linux: ack and prepare), then RUN */
	if ((r = refill(d, st)) != 0)
		goto fail;
	len = asm_pkt_run(pkt, st->session);
	if ((r = asm_cmd(&d->q, pkt, len, "run")) != 0)
		goto fail;
	st->running = true;
	st->state = ST_RUNNING;
	st->starts++;
	fma_log("pcm: %s started: session %d, %u Hz, %u ch, %u blocks of %u "
	    "bytes at %#jx, COPP %u on port %#x, %u blocks queued",
	    chname(st->chan), st->session, st->rate, st->channels, st->blkcnt,
	    st->blksz, (uintmax_t)st->dspaddr, st->copp, st->afe_port,
	    st->submitted);
	return (0);
fail:
	fma_log("pcm: %s: the ADSP's side failed at step %d: %s (the kernel "
	    "consumes the sound at its rate instead)", chname(st->chan), step,
	    q6_err(r));
	close_session(d, st, step);
	st->state = ST_FAILED;
	st->errors++;
	fail_kernel(d, st, r < 0 ? r : -EIO);
	return (r);
}

static void
stop(struct audiod *d, struct stream *st)
{
	if (st->state != ST_RUNNING) {
		st->state = ST_IDLE;
		return;
	}
	/* CLOSE returns the buffers in flight (their basic results) */
	close_session(d, st, 4);
	st->state = ST_IDLE;
	st->running = false;
	fma_log("pcm: %s stopped after %u blocks (%u done events)",
	    chname(st->chan), st->hw, st->dones);
}

void
pcm_event(struct audiod *d, const struct pro1x_pcm_event *ev)
{
	struct stream *st;

	switch (ev->type) {
	case PRO1X_PCM_EV_HELLO:
		if (ev->chan < PRO1X_PCM_NCHAN)
			fma_log("pcm: %s buffer at %#jx, for the ADSP %#jx, %u "
			    "bytes", chname((int)ev->chan), (uintmax_t)ev->paddr,
			    (uintmax_t)ev->dspaddr, ev->bufsize);
		return;
	case PRO1X_PCM_EV_START:
		if (ev->chan >= PRO1X_PCM_NCHAN)
			return;
		st = &d->st[ev->chan];
		if (st->state == ST_RUNNING)
			stop(d, st);		/* a new start without a stop */
		st->chan = (int)ev->chan;
		st->session = (int)ev->chan + 1;
		st->gen = ev->gen;
		st->rate = ev->rate;
		st->channels = ev->channels;
		st->bits = ev->bits;
		st->blksz = ev->blksz;
		st->blkcnt = ev->blkcnt;
		st->dspaddr = ev->dspaddr;
		if (st->blksz == 0 || st->blkcnt < 2 || st->rate == 0) {
			fma_log("pcm: %s: start with %u x %u bytes at %u Hz "
			    "ignored", chname(st->chan), st->blkcnt, st->blksz,
			    st->rate);
			st->state = ST_FAILED;
			fail_kernel(d, st, -EINVAL);
			return;
		}
		start(d, st, ev->ready);
		return;
	case PRO1X_PCM_EV_STOP:
		if (ev->chan >= PRO1X_PCM_NCHAN)
			return;
		st = &d->st[ev->chan];
		if (st->gen == ev->gen)
			stop(d, st);
		return;
	case PRO1X_PCM_EV_MIXER:
		backend_mixer(d, ev->dev, ev->left);
		return;
	case PRO1X_PCM_EV_RECSRC:
		d->recsrc = ev->recsrc;
		/* SOUND_MASK_LINE: the headset's microphone on the WCD */
		snprintf(d->recroute, sizeof(d->recroute), "%s",
		    ev->recsrc & (1u << 6) ? "amic" : "mic");
		return;
	}
}

static struct stream *
by_session(struct audiod *d, const uint8_t *pkt)
{
	int session = apr_dest_port(pkt) >> 8;
	unsigned i;

	for (i = 0; i < PRO1X_PCM_NCHAN; i++)
		if (d->st[i].session == session && d->st[i].state == ST_RUNNING)
			return (&d->st[i]);
	return (NULL);
}

/* ASM packets nobody waits for: the data events */
void
pcm_asm_event(struct audiod *d, const uint8_t *pkt, size_t len)
{
	struct pro1x_pcm_advance a;
	struct stream *st;
	size_t h = apr_hlen(pkt, len);
	uint32_t op = apr_opcode(pkt), status;

	if (h == 0 || (op != ASM_DATA_EVENT_WRITE_DONE_V2 &&
	    op != ASM_DATA_EVENT_READ_DONE_V2))
		return;
	if ((st = by_session(d, pkt)) == NULL)
		return;
	/* write done: {addr lsw, msw, handle, status}; read: status first */
	if (len < h + 16)
		return;
	status = op == ASM_DATA_EVENT_WRITE_DONE_V2 ? le32dec_(pkt + h + 12) :
	    le32dec_(pkt + h);
	if (status != 0 && st->errors++ < 5)
		fma_log("pcm: %s: block %u: ADSP status %#x", chname(st->chan),
		    apr_token(pkt) & 0xffff, status);
	if ((apr_token(pkt) & 0xffff) != st->hw % st->blkcnt && st->errors++ < 5)
		fma_log("pcm: %s: block %u done, %u expected", chname(st->chan),
		    apr_token(pkt) & 0xffff, st->hw % st->blkcnt);
	st->dones++;
	memset(&a, 0, sizeof(a));
	a.chan = (uint32_t)st->chan;
	a.gen = st->gen;
	a.blocks = 1;
	if (d->pcm->advance(d->pcm->ctx, &a) != 0 || !a.running) {
		/* stopped meanwhile: its STOP event follows */
		return;
	}
	st->hw = a.hw;
	st->ready = a.ready;
	if (st->chan == PRO1X_PCM_PLAY)
		d->blocks_out++;
	else
		d->blocks_in++;
	if (refill(d, st) != 0 && st->errors++ < 5)
		fma_log("pcm: %s: a block could not be sent", chname(st->chan));
}

void
pcm_stop_all(struct audiod *d)
{
	unsigned i;

	for (i = 0; i < PRO1X_PCM_NCHAN; i++)
		stop(d, &d->st[i]);
}
