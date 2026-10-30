/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The replay sequences (freebsd/audio/README.md, "The sequences"): text,
 * one action per line, in [sections] that the daemon runs by name
 * (power, clocks, rx-up, hph-up, hph-down, mic-up, ...). They are written
 * by tools/trace2seq.py from the Leste trace (test/leste-audio-trace.sh)
 * or by hand from Linux's code, and shared with the FM radio's stage 3c
 * (freebsd/fm/README.md):
 *
 *   w <target> <reg> <val>             register write
 *   u <target> <reg> <mask> <val>      read, change the bits in mask, write
 *   r <target> <reg> [<val> [<mask>]]  read; with val, a differing value is
 *                                      logged
 *   poll <target> <reg> <mask> <val> <ms>  until (v & mask) == val
 *   delay <ms>, udelay <us>
 *   gpio <pin> 0|1                     a TLMM line (the daemon's gpioc)
 *   rail <name> on|off [<uV>]          a regulator at the RPM (l9, l14, ...)
 *   clock <id|name> <hz>               an LPASS clock at the AFE, 0 off
 *   vote <block|name> on|off           an LPASS core block (AFE vote)
 *   afe start|stop                     the back end's AFE port, here
 *   swr <words...>                     SoundWire: "init swr1", "enum swr0 3",
 *                                      "stream swr1 on 1:3 2:1" (swr.c),
 *                                      "wcd-init" (the WCD9370's probe)
 *   call <section>                     another section, then on
 *   mark <text>                        a line in the log
 *
 * Targets: the LPASS windows of /dev/qcom_lpass by name (rxmacro, txmacro,
 * vamacro, swr0, swr1, lpass_tlmm, lpass_tlmm_slew, lpass_audiocc,
 * lpasscc; byte offsets,
 * 32-bit accesses), wcd (the WCD9370's registers, over SoundWire swr0 as
 * Linux's wcd937x regmap), wcdrx (its RX device on swr1), aw0 and aw1 (the
 * AW87339s at I2C 0x59 and 0x5a). Numbers are C style; a value may be
 * $name, which the daemon fills in (volumes). '#' starts a comment.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audiod.h"

static const char *const tgt_names[TGT_N] = {
	[TGT_RXMACRO] = "rxmacro", [TGT_TXMACRO] = "txmacro",
	[TGT_VAMACRO] = "vamacro", [TGT_SWR0] = "swr0", [TGT_SWR1] = "swr1",
	[TGT_LPASS_TLMM] = "lpass_tlmm", [TGT_LPASS_TLMM_SLEW] = "lpass_tlmm_slew",
	[TGT_LPASS_AUDIOCC] = "lpass_audiocc",
	[TGT_LPASSCC] = "lpasscc", [TGT_WCD] = "wcd", [TGT_WCDRX] = "wcdrx",
	[TGT_AW0] = "aw0", [TGT_AW1] = "aw1",
};

/* the q6afe clocks and blocks by the names Linux's clock framework shows */
static const struct { const char *name; uint32_t id; } clk_names[] = {
	{ "LPASS_CLK_ID_TX_CORE_MCLK", Q6AFE_LPASS_CLK_ID_TX_CORE_MCLK },
	{ "LPASS_CLK_ID_TX_CORE_NPL_MCLK", Q6AFE_LPASS_CLK_ID_TX_CORE_NPL_MCLK },
	{ "LPASS_CLK_ID_RX_CORE_MCLK", Q6AFE_LPASS_CLK_ID_RX_CORE_MCLK },
	{ "LPASS_CLK_ID_RX_CORE_NPL_MCLK", Q6AFE_LPASS_CLK_ID_RX_CORE_NPL_MCLK },
	{ "LPASS_CLK_ID_VA_CORE_MCLK", 0x30b },
	{ "LPASS_CLK_ID_VA_CORE_2X_MCLK", 0x310 },
};
static const struct { const char *name; uint32_t block; } blk_names[] = {
	{ "LPASS_HW_MACRO", Q6AFE_LPASS_CORE_HW_MACRO_BLOCK },
	{ "LPASS_HW_DCODEC", Q6AFE_LPASS_CORE_HW_DCODEC_BLOCK },
};

const char *
tgt_name(int t)
{
	return (t >= 0 && t < TGT_N ? tgt_names[t] : "?");
}

static int
tgt_parse(const char *s)
{
	int i;

	for (i = 0; i < TGT_N; i++)
		if (strcmp(s, tgt_names[i]) == 0)
			return (i);
	return (-1);
}

static char *
xstrdup(const char *s)
{
	char *p = strdup(s);

	if (p == NULL)
		abort();
	return (p);
}

static int
err(char *e, size_t elen, const char *name, int lineno, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(e, elen, "%s:%d: ", name, lineno);
	if (n < 0 || (size_t)n >= elen)
		return (-EINVAL);
	va_start(ap, fmt);
	vsnprintf(e + n, elen - n, fmt, ap);
	va_end(ap);
	return (-EINVAL);
}

/* a number, or $name (then *var points to it) */
static int
num(const char *s, uint32_t *v, char **var)
{
	char *end;
	unsigned long long x;

	if (s == NULL)
		return (-1);
	if (s[0] == '$' && s[1] != '\0') {
		if (var == NULL || *var != NULL)
			return (-1);
		*var = xstrdup(s + 1);
		*v = 0;
		return (0);
	}
	errno = 0;
	x = strtoull(s, &end, 0);
	if (errno != 0 || *end != '\0' || x > 0xffffffffull)
		return (-1);
	*v = (uint32_t)x;
	return (0);
}

static void
add_line(struct seq *s, const struct seq_line *l)
{
	struct seq_line *n;

	n = realloc(s->lines, (s->nlines + 1) * sizeof(*n));
	if (n == NULL)
		abort();
	s->lines = n;
	s->lines[s->nlines++] = *l;
}

int
seq_parse(struct seq *s, const char *text, const char *name, char *e,
    size_t elen)
{
	const char *p = text, *nl;
	char line[512], *w[12], *c;
	struct seq_line l;
	struct seq_sect *cur = NULL;
	int lineno = 0, nw, t;
	size_t len;

	memset(s, 0, sizeof(*s));
	s->file = xstrdup(name);
	while (*p != '\0') {
		nl = strchr(p, '\n');
		len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		lineno++;
		if (len >= sizeof(line))
			return (err(e, elen, name, lineno, "line too long"));
		memcpy(line, p, len);
		line[len] = '\0';
		p += len + (nl != NULL);
		if ((c = strchr(line, '#')) != NULL)
			*c = '\0';
		for (nw = 0, c = strtok(line, " \t\r"); c != NULL && nw < 12;
		    c = strtok(NULL, " \t\r"))
			w[nw++] = c;
		if (nw == 0)
			continue;
		if (w[0][0] == '[') {
			len = strlen(w[0]);
			if (nw != 1 || len < 3 || w[0][len - 1] != ']')
				return (err(e, elen, name, lineno, "bad section"));
			if (s->nsect == SEQ_MAXSECT)
				return (err(e, elen, name, lineno,
				    "too many sections"));
			w[0][len - 1] = '\0';
			if (seq_find(s, w[0] + 1) != NULL)
				return (err(e, elen, name, lineno,
				    "section %s twice", w[0] + 1));
			cur = &s->sect[s->nsect++];
			cur->name = xstrdup(w[0] + 1);
			cur->first = s->nlines;
			cur->count = 0;
			continue;
		}
		if (cur == NULL)
			return (err(e, elen, name, lineno,
			    "a line before the first section"));
		if (s->nlines >= SEQ_MAXLINES)
			return (err(e, elen, name, lineno, "too many lines"));
		memset(&l, 0, sizeof(l));
		l.lineno = lineno;
		l.target = -1;
		if (strcmp(w[0], "w") == 0 || strcmp(w[0], "u") == 0 ||
		    strcmp(w[0], "r") == 0 || strcmp(w[0], "poll") == 0) {
			l.op = w[0][0] == 'w' ? SEQ_W : w[0][0] == 'u' ? SEQ_U :
			    w[0][0] == 'r' ? SEQ_R : SEQ_POLL;
			if (nw < 3 || (t = tgt_parse(w[1])) < 0 ||
			    num(w[2], &l.reg, NULL) != 0)
				return (err(e, elen, name, lineno,
				    "%s: target and register", w[0]));
			l.target = t;
			if ((l.op == SEQ_W && (nw != 4 ||
			    num(w[3], &l.val, &l.text) != 0)) ||
			    (l.op == SEQ_U && (nw != 5 || num(w[3], &l.mask, NULL) ||
			    num(w[4], &l.val, &l.text))) ||
			    (l.op == SEQ_R && (nw > 5 || (nw >= 4 &&
			    num(w[3], &l.val, NULL)) || (nw == 5 &&
			    num(w[4], &l.mask, NULL)))) ||
			    (l.op == SEQ_POLL && (nw != 6 || num(w[3], &l.mask,
			    NULL) || num(w[4], &l.val, NULL) || num(w[5], &l.arg,
			    NULL))))
				return (err(e, elen, name, lineno, "%s: values",
				    w[0]));
			if (l.op == SEQ_R) {
				l.arg = nw >= 4;	/* compare */
				if (nw < 5)
					l.mask = 0xffffffff;
			}
		} else if (strcmp(w[0], "delay") == 0 ||
		    strcmp(w[0], "udelay") == 0) {
			l.op = SEQ_DELAY;
			if (nw != 2 || num(w[1], &l.arg, NULL) != 0)
				return (err(e, elen, name, lineno, "delay"));
			if (w[0][0] == 'd')
				l.arg *= 1000;		/* in us */
		} else if (strcmp(w[0], "gpio") == 0) {
			l.op = SEQ_GPIO;
			if (nw != 3 || num(w[1], &l.reg, NULL) ||
			    num(w[2], &l.val, NULL) || l.val > 1)
				return (err(e, elen, name, lineno, "gpio"));
		} else if (strcmp(w[0], "rail") == 0) {
			l.op = SEQ_RAIL;
			if (nw < 3 || nw > 4 || (strcmp(w[2], "on") &&
			    strcmp(w[2], "off")) || (nw == 4 &&
			    num(w[3], &l.arg, NULL)))
				return (err(e, elen, name, lineno, "rail"));
			l.text = xstrdup(w[1]);
			l.val = strcmp(w[2], "on") == 0;
		} else if (strcmp(w[0], "clock") == 0) {
			size_t i;

			l.op = SEQ_CLOCK;
			if (nw != 3 || num(w[2], &l.val, NULL) != 0)
				return (err(e, elen, name, lineno, "clock"));
			if (num(w[1], &l.reg, NULL) != 0) {
				for (i = 0; i < nitems(clk_names); i++)
					if (strcmp(w[1], clk_names[i].name) == 0)
						break;
				if (i == nitems(clk_names))
					return (err(e, elen, name, lineno,
					    "clock %s unknown", w[1]));
				l.reg = clk_names[i].id;
			}
		} else if (strcmp(w[0], "vote") == 0) {
			size_t i;

			l.op = SEQ_VOTE;
			if (nw != 3 || (strcmp(w[2], "on") && strcmp(w[2], "off")))
				return (err(e, elen, name, lineno, "vote"));
			if (num(w[1], &l.reg, NULL) != 0) {
				for (i = 0; i < nitems(blk_names); i++)
					if (strcmp(w[1], blk_names[i].name) == 0)
						break;
				if (i == nitems(blk_names))
					return (err(e, elen, name, lineno,
					    "block %s unknown", w[1]));
				l.reg = blk_names[i].block;
			}
			l.val = strcmp(w[2], "on") == 0;
		} else if (strcmp(w[0], "afe") == 0) {
			l.op = SEQ_AFE;
			if (nw != 2 || (strcmp(w[1], "start") &&
			    strcmp(w[1], "stop")))
				return (err(e, elen, name, lineno, "afe"));
			l.text = xstrdup(w[1]);
		} else if (strcmp(w[0], "swr") == 0 || strcmp(w[0], "mark") == 0) {
			char buf[512];
			int i;

			l.op = w[0][0] == 's' ? SEQ_SWR : SEQ_MARK;
			if (nw < 2)
				return (err(e, elen, name, lineno, "%s", w[0]));
			buf[0] = '\0';
			for (i = 1; i < nw; i++) {
				strlcat(buf, w[i], sizeof(buf));
				if (i + 1 < nw)
					strlcat(buf, " ", sizeof(buf));
			}
			l.text = xstrdup(buf);
		} else if (strcmp(w[0], "call") == 0) {
			l.op = SEQ_CALL;
			if (nw != 2)
				return (err(e, elen, name, lineno, "call"));
			l.text = xstrdup(w[1]);
		} else
			return (err(e, elen, name, lineno, "unknown %s", w[0]));
		add_line(s, &l);
		cur->count++;
	}
	return (0);
}

int
seq_load(struct seq *s, const char *path, char *e, size_t elen)
{
	FILE *f;
	char *buf;
	long n;
	int r;

	if ((f = fopen(path, "r")) == NULL) {
		snprintf(e, elen, "%s: %s", path, strerror(errno));
		return (-errno);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	if (n < 0 || n > 4 * 1024 * 1024 || (buf = malloc((size_t)n + 1)) == NULL) {
		fclose(f);
		snprintf(e, elen, "%s: too big", path);
		return (-EFBIG);
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		snprintf(e, elen, "%s: read error", path);
		return (-EIO);
	}
	fclose(f);
	buf[n] = '\0';
	r = seq_parse(s, buf, path, e, elen);
	free(buf);
	if (r != 0)
		seq_free(s);
	return (r);
}

void
seq_free(struct seq *s)
{
	unsigned i;

	for (i = 0; i < s->nlines; i++)
		free(s->lines[i].text);
	for (i = 0; i < s->nsect; i++)
		free(s->sect[i].name);
	free(s->lines);
	free(s->file);
	memset(s, 0, sizeof(*s));
}

const struct seq_sect *
seq_find(const struct seq *s, const char *name)
{
	unsigned i;

	for (i = 0; i < s->nsect; i++)
		if (strcmp(s->sect[i].name, name) == 0)
			return (&s->sect[i]);
	return (NULL);
}

/* --- running a section --------------------------------------------------- */

static int
reg_read(struct codec_io *io, int t, uint32_t reg, uint32_t *v)
{
	uint8_t b = 0;
	int r;

	if (t < TGT_NMMIO) {
		if (io->win[t] == NULL)
			return (-ENXIO);
		*v = mmio_rd(io->win[t], reg);
		return (0);
	}
	if (t == TGT_WCD || t == TGT_WCDRX)
		r = io->sdw_read != NULL ? io->sdw_read(io->ctx, t, reg, &b) :
		    -ENXIO;
	else
		r = io->i2c_read != NULL ? io->i2c_read(io->ctx, t,
		    (uint8_t)reg, &b) : -ENXIO;
	*v = b;
	return (r);
}

static int
reg_write(struct codec_io *io, int t, uint32_t reg, uint32_t v)
{
	if (t < TGT_NMMIO) {
		if (io->win[t] == NULL)
			return (-ENXIO);
		mmio_wr(io->win[t], reg, v);
		return (0);
	}
	if (v > 0xff)
		return (-EINVAL);
	if (t == TGT_WCD || t == TGT_WCDRX)
		return (io->sdw_write != NULL ? io->sdw_write(io->ctx, t, reg,
		    (uint8_t)v) : -ENXIO);
	return (io->i2c_write != NULL ? io->i2c_write(io->ctx, t, (uint8_t)reg,
	    (uint8_t)v) : -ENXIO);
}

static int
run(const struct seq *s, const struct seq_sect *sec, struct codec_io *io,
    int depth)
{
	const struct seq_line *l;
	const struct seq_sect *sub;
	uint32_t v, val;
	uint64_t until;
	unsigned i;
	int r, errors = 0;

	for (i = 0; i < sec->count; i++) {
		l = &s->lines[sec->first + i];
		val = l->val;
		if (l->text != NULL && (l->op == SEQ_W || l->op == SEQ_U)) {
			if (io->var == NULL || io->var(io->ctx, l->text, &val)) {
				fma_log("seq %s:%d: no value for $%s", s->file,
				    l->lineno, l->text);
				errors++;
				continue;
			}
		}
		r = 0;
		switch (l->op) {
		case SEQ_W:
			r = reg_write(io, l->target, l->reg, val);
			if (io->trace)
				fma_log("seq: w %s %#x %#x", tgt_name(l->target),
				    l->reg, val);
			break;
		case SEQ_U:
			if ((r = reg_read(io, l->target, l->reg, &v)) == 0)
				r = reg_write(io, l->target, l->reg,
				    (v & ~l->mask) | (val & l->mask));
			if (io->trace)
				fma_log("seq: u %s %#x %#x: %#x -> %#x",
				    tgt_name(l->target), l->reg, l->mask, v,
				    (v & ~l->mask) | (val & l->mask));
			break;
		case SEQ_R:
			if ((r = reg_read(io, l->target, l->reg, &v)) != 0)
				break;
			if (l->arg && (v & l->mask) != (l->val & l->mask))
				fma_log("seq %s:%d: %s %#x is %#x, Linux had %#x",
				    s->file, l->lineno, tgt_name(l->target),
				    l->reg, v, l->val);
			else if (io->trace)
				fma_log("seq: r %s %#x = %#x", tgt_name(l->target),
				    l->reg, v);
			break;
		case SEQ_POLL:
			until = fma_now_ms() + l->arg;
			for (;;) {
				if ((r = reg_read(io, l->target, l->reg, &v)) != 0)
					break;
				if ((v & l->mask) == l->val)
					break;
				if (fma_now_ms() >= until) {
					fma_log("seq %s:%d: %s %#x stays %#x "
					    "(want %#x/%#x)", s->file, l->lineno,
					    tgt_name(l->target), l->reg, v,
					    l->val, l->mask);
					r = -ETIMEDOUT;
					break;
				}
				fma_sleep_ms(0);
			}
			break;
		case SEQ_DELAY:
			if (io->delay_us != NULL)
				io->delay_us(io->ctx, l->arg);
			break;
		case SEQ_GPIO:
			r = io->gpio != NULL ? io->gpio(io->ctx, l->reg,
			    (int)l->val) : -ENXIO;
			break;
		case SEQ_RAIL:
			r = io->rail != NULL ? io->rail(io->ctx, l->text,
			    l->val != 0, l->arg) : -ENXIO;
			break;
		case SEQ_CLOCK:
			r = io->clock != NULL ? io->clock(io->ctx, l->reg,
			    l->val) : -ENXIO;
			break;
		case SEQ_VOTE:
			r = io->vote != NULL ? io->vote(io->ctx, l->reg,
			    l->val != 0) : -ENXIO;
			break;
		case SEQ_AFE:
			r = io->afe != NULL ? io->afe(io->ctx, l->text) : -ENXIO;
			break;
		case SEQ_SWR:
			r = io->swr != NULL ? io->swr(io->ctx, l->text) : -ENXIO;
			break;
		case SEQ_MARK:
			fma_log("seq: %s", l->text);
			break;
		case SEQ_CALL:
			if (depth > 8 || (sub = seq_find(s, l->text)) == NULL) {
				fma_log("seq %s:%d: no section %s (or too deep)",
				    s->file, l->lineno, l->text);
				r = -ENOENT;
				break;
			}
			r = run(s, sub, io, depth + 1);
			if (r > 0) {
				errors += r;
				r = 0;
			}
			break;
		}
		if (r < 0) {
			errors++;
			fma_log("seq %s:%d: %s", s->file, l->lineno,
			    strerror(-r));
		}
	}
	return (errors);
}

/* the errors (0: all done); -ENOENT: no such section */
int
seq_run(const struct seq *s, const char *section, struct codec_io *io)
{
	const struct seq_sect *sec;

	if (s == NULL || (sec = seq_find(s, section)) == NULL)
		return (-ENOENT);
	fma_dbg("seq: [%s], %u lines", section, sec->count);
	return (run(s, sec, io, 0));
}
