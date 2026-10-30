/*
 * qcom_rnd's qcom,prng-ee read (patches/0038, sys/dev/qcom_rnd/qcom_rnd_ee.h)
 * on the PC, against a model of the PRNG's window and of its clock in the
 * GCC (freebsd/rng/README.md):
 *
 *   WORK=... bash freebsd/rng/build.sh host
 *
 * Each read votes the clock on and back (the GCC's other votes kept), never
 * touches the PRNG without the vote nor its configuration (TrustZone's), and
 * fails on a word of zero, on no word for 10 ms, on a word as the one before
 * and on a clock that does not come on; hardware gating and the NOC FSM's
 * status count as on, as in Linux's clk-branch.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dev/qcom_rnd/qcom_rnd_ee.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; printf("%s: ", (c) ? "ok" : "FAIL"); printf(__VA_ARGS__); \
	printf("\n"); if (!(c)) fails++; } while (0)

static const struct qcom_rnd_clk clk = { "qcom,gcc-sm6115", 0x79004, 13, 0x21004 };

static struct {
	uint32_t vote, cbcr_extra;
	bool hwctl, stuck;
	const uint32_t *words;
	int nwords, next, avail_after, polls;
	bool never;
	unsigned long us;
	int bad;
	char why[96];
} m;

static bool voted(void) { return (m.vote >> clk.bit) & 1; }

static uint32_t prng_rd(void *ctx, uint32_t off)
{
	if (!voted() || off >= 0x100) {
		m.bad++;
		snprintf(m.why, sizeof(m.why), "PRNG +0x%x %s", off, off >= 0x100 ? "(configuration)" : "without the vote");
	}
	if (off == QCOM_RND_EE_STATUS) {
		if (m.never || m.next >= m.nwords)
			return (0);
		return (++m.polls > m.avail_after ? QCOM_RND_EE_DATA_AVAIL : 0);
	}
	if (off == QCOM_RND_EE_DATA_OUT) {
		m.polls = 0;
		return (m.next < m.nwords ? m.words[m.next++] : 0);
	}
	return (0);
}

static uint32_t gcc_rd(void *ctx, uint32_t off)
{
	if (off == clk.vote)
		return (m.vote);
	if (off == clk.cbcr)
		return (m.cbcr_extra | (m.hwctl ? QCOM_RND_CBCR_HW_CTL : 0) |
		    (voted() && !m.stuck ? 0 : QCOM_RND_CBCR_CLK_OFF));
	m.bad++;
	snprintf(m.why, sizeof(m.why), "GCC read +0x%x", off);
	return (0);
}

static void gcc_wr(void *ctx, uint32_t off, uint32_t v)
{
	if (off == clk.vote) {
		m.vote = v;
		return;
	}
	m.bad++;
	snprintf(m.why, sizeof(m.why), "GCC write +0x%x", off);
}

static void delay(void *ctx, int us) { m.us += us; }

static struct qcom_rnd_ee e;

static void model(const uint32_t *w, int n)
{
	memset(&m, 0, sizeof(m));
	m.vote = 0x00c00011;
	m.words = w;
	m.nwords = n;
	m.avail_after = 2;
	e = (struct qcom_rnd_ee){ .prng_rd = prng_rd, .gcc_rd = gcc_rd, .gcc_wr = gcc_wr,
	    .delay = delay, .clk = &clk };
}

int main(void)
{
	static const uint32_t w[] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff01 };
	static const uint32_t z[] = { 0x11111111, 0 }, r[] = { 0x5a5a5a5a, 0x5a5a5a5a };
	uint8_t out[16];
	size_t n;

	model(w, 4);
	n = qcom_rnd_ee_read(&e, out, 8);
	CHECK(n == 8 && !memcmp(out, w, 8) && e.words == 2, "8 bytes: two words (FreeBSD's harvest size)");
	CHECK(m.vote == 0x00c00011 && m.bad == 0, "the vote back, the other votes kept, nothing refused %s", m.why);
	n = qcom_rnd_ee_read(&e, out, 6);
	CHECK(n == 6 && !memcmp(out, w + 2, 6), "6 bytes: the next two words, half of the second");

	model(w, 4);
	m.hwctl = true, m.stuck = true;
	CHECK(qcom_rnd_ee_read(&e, out, 4) == 4 && e.clock_stuck == 0, "hardware gating: CLK_OFF not waited for");

	model(w, 4);
	m.cbcr_extra = (uint32_t)QCOM_RND_CBCR_FSM_ON << 28, m.stuck = true;
	CHECK(qcom_rnd_ee_read(&e, out, 4) == 4, "the NOC FSM's status on: on");

	model(w, 4);
	m.stuck = true;
	n = qcom_rnd_ee_read(&e, out, 4);
	CHECK(n == 0 && e.clock_stuck == 1 && !voted() && m.bad == 0 && m.us >= 200 && m.us < 300,
	    "a clock that does not come on: 200 us, the PRNG not touched, the vote back");

	model(z, 2);
	memset(out, 0xaa, sizeof(out));
	n = qcom_rnd_ee_read(&e, out, 8);
	CHECK(n == 0 && e.zeros == 1 && out[0] == 0 && out[7] == 0 && !voted(), "a word of zero: the read fails, cleared");

	model(r, 2);
	n = qcom_rnd_ee_read(&e, out, 8);
	CHECK(n == 0 && e.repeats == 1, "the same word twice: the read fails (continuous test)");
	model(r, 2);
	n = qcom_rnd_ee_read(&e, out, 4);
	CHECK(n == 4 && qcom_rnd_ee_read(&e, out, 4) == 0 && e.repeats == 1, "... also across two reads");

	model(w, 4);
	m.never = true;
	n = qcom_rnd_ee_read(&e, out, 4);
	CHECK(n == 0 && e.timeouts == 1 && m.us >= 10000 && m.us <= 10300 && !voted(),
	    "no word: given up after 10 ms (%lu us)", m.us);

	model(w, 4);
	e.clk = NULL;
	m.vote = 1u << clk.bit;		/* someone else's vote: the model's PRNG answers */
	CHECK(qcom_rnd_ee_read(&e, out, 4) == 4 && m.vote == 1u << clk.bit, "no clock of its own: no vote written");

	printf("%d of %d checks passed\n", checks - fails, checks);
	return (fails != 0);
}
