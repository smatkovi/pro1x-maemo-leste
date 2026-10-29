/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests of pro1x-fmd's protocol core (qcabt.c, qcafm.c) on the PC:
 * the firmware files' checks and the NVM patch on linux-firmware's
 * qca/cmbtfw12.tlv and qca/cmnv12.bin, the file names from the version
 * the Pro1X reports, the H4 framer with in-band sleep bytes at every
 * split, the FM event parsers.
 *
 *   test_qcabt <directory with cmbtfw12.tlv and cmnv12.bin>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcabt.h"
#include "qcafm.h"

static int failures;

#define	CHECK(cond) do {						\
	if (!(cond)) {							\
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
		failures++;						\
	}								\
} while (0)

static uint8_t *
load(const char *dir, const char *name, size_t *len)
{
	char path[512];
	FILE *f;
	uint8_t *p;
	long n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if ((f = fopen(path, "rb")) == NULL) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	p = malloc((size_t)n);
	if (fread(p, 1, (size_t)n, f) != (size_t)n)
		exit(2);
	fclose(f);
	*len = (size_t)n;
	return (p);
}

/* --- a transport that records what is written ---------------------------- */

static uint8_t written[4096];
static size_t nwritten;
static uint64_t clock_ms;

static int
m_write(void *arg, const void *buf, size_t len)
{
	(void)arg;
	if (nwritten + len <= sizeof(written)) {
		memcpy(written + nwritten, buf, len);
		nwritten += len;
	}
	return (0);
}

static int
m_read(void *arg, void *buf, size_t len, int timeout_ms)
{
	(void)arg, (void)buf, (void)len;
	clock_ms += timeout_ms > 0 ? (uint64_t)timeout_ms : 1;
	return (0);
}

static int m_speed(void *a, unsigned int b) { (void)a, (void)b; return (0); }
static int m_flow(void *a, bool on) { (void)a, (void)on; return (0); }
static int m_drain(void *a) { (void)a; return (0); }
static void m_flush(void *a) { (void)a; }
static uint64_t m_now(void *a) { (void)a; return (clock_ms); }
static void m_sleep(void *a, unsigned int ms) { (void)a; clock_ms += ms; }
static void m_log(void *a, const char *m) { (void)a; (void)m; }
static int
m_fw(void *a, const char *n, uint8_t **d, size_t *l)
{
	(void)a, (void)n, (void)d, (void)l;
	return (-ENOENT);
}

static const struct qbt_ops mops = {
	m_write, m_read, m_speed, m_flow, m_drain, m_flush, m_now, m_sleep,
	m_log, m_fw,
};

static int fm_events;
static uint8_t last_evt;

static void
count_fm(void *arg, uint8_t evt, const uint8_t *p, uint8_t plen)
{
	(void)arg, (void)p, (void)plen;
	fm_events++;
	last_evt = evt;
}

static void
test_files(const char *dir)
{
	size_t len, i, segs;
	uint8_t *ram, *nvm, *nvm115;
	int mode, r;

	ram = load(dir, "cmbtfw12.tlv", &len);
	r = qbt_tlv_check(ram, len, 1, qbt_baud_code(3000000), &mode);
	CHECK(r == 0);
	CHECK(mode == QCA_SKIP_EVT_VSE_CC);
	segs = (len + MAX_SIZE_PER_TLV_SEGMENT - 1) / MAX_SIZE_PER_TLV_SEGMENT;
	printf("rampatch: %zu bytes, download mode %d, %zu segments\n", len,
	    mode, segs);
	CHECK(len == 47307 && segs == 195);
	free(ram);

	nvm = load(dir, "cmnv12.bin", &len);
	nvm115 = malloc(len);
	memcpy(nvm115, nvm, len);
	/* tag 17 of the inner type 2 set: at 8 + 12 + 6 + 12 = 38 + 12 */
	CHECK(nvm[0] == 4 && nvm[4] == 2);
	i = 8;
	while (i + 12 < len && (nvm[i] | nvm[i + 1] << 8) != 17)
		i += 12 + (size_t)(nvm[i + 2] | nvm[i + 3] << 8);
	CHECK(i + 18 <= len);
	printf("NVM tag 17 before: %02x %02x %02x %02x %02x %02x\n",
	    nvm[i + 12], nvm[i + 13], nvm[i + 14], nvm[i + 15], nvm[i + 16],
	    nvm[i + 17]);
	CHECK(nvm[i + 12] == 0x15 && nvm[i + 13] == 0x11);
	r = qbt_tlv_check(nvm, len, 2, qbt_baud_code(3000000), &mode);
	CHECK(r == 0 && mode == QCA_SKIP_EVT_NONE);
	printf("NVM tag 17 after (3 Mbaud): %02x %02x %02x %02x %02x %02x\n",
	    nvm[i + 12], nvm[i + 13], nvm[i + 14], nvm[i + 15], nvm[i + 16],
	    nvm[i + 17]);
	/* in-band sleep on, the rate 3000000 (14) in byte 1, byte 2 kept */
	CHECK(nvm[i + 12] == 0x95 && nvm[i + 13] == 14 && nvm[i + 14] == 0x22);
	r = qbt_tlv_check(nvm115, len, 2, qbt_baud_code(115200), &mode);
	CHECK(r == 0 && nvm115[i + 12] == 0x95 && nvm115[i + 13] == 0);
	/* nothing else changed but tag 17 and tag 27's first byte */
	for (size_t k = 0; k < len; k++)
		if (k != i + 12 && k != i + 13 && nvm[k] != nvm115[k])
			CHECK(0);
	segs = (len + MAX_SIZE_PER_TLV_SEGMENT - 1) / MAX_SIZE_PER_TLV_SEGMENT;
	printf("NVM: %zu bytes, %zu segments\n", len, segs);
	CHECK(segs == 12);
	/* truncated files are refused */
	CHECK(qbt_tlv_check(nvm, 100, 2, 14, &mode) == -EINVAL);
	CHECK(qbt_tlv_check(nvm, 20, 1, 14, &mode) == -EINVAL);
	free(nvm);
	free(nvm115);
}

static void
test_version(void)
{
	/* the Pro1X: SoC 0x40070120, ROM 0x0102 (docs/fm-radio.md) */
	struct qbt_version v = { .soc_id = 0x40070120, .rom_ver = 0x0102 };

	CHECK(qbt_rom_version(&v) == 0x12);
	CHECK(qbt_baud_code(3000000) == 14);
	CHECK(qbt_baud_code(3200000) == 17);
	CHECK(qbt_baud_code(115200) == 0);
}

/* a stream of frames, fed in two pieces at every split point */
static void
test_framer(void)
{
	static const uint8_t stream[] = {
		IBS_WAKE_IND,
		/* Command Complete for HCI_RESET, status 0 */
		0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00,
		0x99,	/* noise */
		/* an FM station name event: 16 bytes */
		0x14, 0x09, 0x10, 0xd3, 0x01, 0x0a, 0, 0, 0, 0, 0x01,
		'R', 'A', 'D', 'I', 'O', ' ', ' ', ' ',
		/* ACL, 3 bytes */
		0x02, 0x01, 0x20, 0x03, 0x00, 0xaa, 0xbb, 0xcc,
		IBS_SLEEP_IND,
		/* FM Command Complete for FM_TUNE */
		0x14, 0x0f, 0x04, 0x01, 0x01, 0x54, 0x00,
	};
	struct qbt bt;
	size_t split;

	for (split = 0; split <= sizeof(stream); split++) {
		qbt_init(&bt, &mops, NULL, 3000000);
		bt.fm_cb = count_fm;
		fm_events = 0;
		nwritten = 0;
		bt.wait_op = HCI_OP_RESET;
		bt.wait_ev = QBT_WAIT_CC;
		bt.fm_wait_op = 0x5401;
		qbt_input(&bt, stream, split);
		qbt_input(&bt, stream + split, sizeof(stream) - split);
		CHECK(bt.done && bt.status == 0 && bt.credits == 1);
		CHECK(bt.fm_done && bt.fm_status == 0);
		CHECK(fm_events == 1 && last_evt == 0x09);
		CHECK(bt.ibs_recv_wakes == 1 && bt.ibs_recv_slps == 1);
		CHECK(bt.rx_errors == 1);
		/* the wake indication answered */
		CHECK(nwritten == 1 && written[0] == IBS_WAKE_ACK);
		CHECK(!bt.rx_awake);
	}
}

static void
test_ibs_send(void)
{
	struct qbt bt;
	uint8_t f[] = { QCA_FM_CMD, 0x0a, 0x4c, 0x00 };
	int r;

	/* asleep: WAKE_IND every 100 ms, ten times, then a timeout */
	qbt_init(&bt, &mops, NULL, 3000000);
	bt.ibs = true;
	bt.up = true;
	nwritten = 0;
	clock_ms = 0;
	r = qbt_send(&bt, f, sizeof(f));
	CHECK(r == -ETIMEDOUT);
	CHECK(bt.ibs_sent_wakes == QBT_WAKE_TRIES && nwritten == QBT_WAKE_TRIES);
	CHECK(written[0] == IBS_WAKE_IND);
	CHECK(bt.tx_state == IBS_TX_ASLEEP);
	/* awake: sent at once; idle for 2 s: the sleep indication */
	bt.tx_state = IBS_TX_AWAKE;
	nwritten = 0;
	clock_ms = 1000;
	r = qbt_send(&bt, f, sizeof(f));
	CHECK(r == 0 && nwritten == sizeof(f));
	clock_ms = 1000 + QBT_TX_IDLE_MS - 1;
	CHECK(qbt_tick(&bt) == 1);
	clock_ms = 1000 + QBT_TX_IDLE_MS;
	CHECK(qbt_tick(&bt) == -1);
	CHECK(bt.tx_state == IBS_TX_ASLEEP && written[nwritten - 1] ==
	    IBS_SLEEP_IND);
	/* a WAKE_ACK while waking: awake */
	bt.tx_state = IBS_TX_WAKING;
	{
		uint8_t ack = IBS_WAKE_ACK;

		qbt_input(&bt, &ack, 1);
	}
	CHECK(bt.tx_state == IBS_TX_AWAKE);
}

static void
test_fm(void)
{
	struct qbt bt;
	struct qfm fm;
	/* tune status: sub-event, then the station at 99.9 MHz */
	uint8_t ts[12] = { 0, 0x3c, 0x86, 0x01, 0x00, 1, (uint8_t)-70, 1, 1,
	    0, 12, 0 };
	uint8_t ps[16] = { 0xd3, 0x01, 0x0a, 0, 0, 0, 0, 1, ' ', 'F', 'M',
	    '4', ' ', ' ', ' ', ' ' };
	uint8_t rt[20] = { 0xd3, 0x01, 0x0a, 0, 0, 'N', 'e', 'w', 's', 0x01,
	    ' ', 'a', 't', ' ', '9', 0x0d, 'x', 'x' };

	qbt_init(&bt, &mops, NULL, 3000000);
	qfm_init(&fm, &bt);
	qfm_event(&fm, FM_EV_TUNE_STATUS, ts, sizeof(ts));
	CHECK(fm.freq == 99900 && fm.st.rssi == -70 && fm.st.stereo == 1 &&
	    fm.st.rds_sync == 1 && fm.st.sinr == 12 && fm.tunes == 1);
	qfm_event(&fm, FM_EV_PS, ps, sizeof(ps));
	CHECK(strcmp(fm.ps, "FM4") == 0 && fm.pty == 10);
	qfm_event(&fm, FM_EV_RT, rt, 18);
	/* up to the carriage return, unprintable as a space */
	CHECK(strcmp(fm.rt, "News  at 9") == 0);
	/* a new tune clears RDS */
	qfm_event(&fm, FM_EV_TUNE_STATUS, ts, sizeof(ts));
	CHECK(fm.ps[0] == '\0' && fm.rt[0] == '\0');
	/* a station name event without names is ignored */
	ps[7] = 0;
	qfm_event(&fm, FM_EV_PS, ps, sizeof(ps));
	CHECK(fm.ps[0] == '\0');
	CHECK(qfm_round(99949) == 99900 && qfm_round(99950) == 100000);
	CHECK(qfm_round(80000) == 87500 && qfm_round(120000) == 108000);
	CHECK(qfm_signal(-120) == 0 && qfm_signal(-80) == 100 &&
	    qfm_signal(-100) == 50 && qfm_signal(-60) == 100);
	/* the receiver off: refused */
	CHECK(qfm_tune(&fm, 99900) == -ENETDOWN);
	CHECK(qfm_seek(&fm, true) == -ENETDOWN);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_qcabt <firmware directory>\n");
		return (2);
	}
	test_files(argv[1]);
	test_version();
	test_framer();
	test_ibs_send();
	test_fm();
	printf("%s: %d failures\n", failures ? "FAIL" : "ok", failures);
	return (failures != 0);
}
