/*
 * Host test of the camera stack's arithmetic: the RCG words camss_gcc.c
 * writes for every entry of Linux's frequency tables (decoded back, every
 * entry of a source must imply the same source rate, i.e. the divider
 * and M/N encodings are read the way they are written), the PM8008 grid
 * voltages for the device tree's ranges, the CSIPHY settle count, the
 * line stride and lane assignment, and that the ioctl structures fit
 * IOCPARM_MAX.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "../sys/arm64/qualcomm/camss/camss_var.h"
#include "../sys/arm64/qualcomm/camss/camss_gcc_sm6115.h"
#include "../sys/arm64/qualcomm/camss/camss_stream.h"
#include "../sys/arm64/qualcomm/camss/qcom_camss_ioctl.h"

uint32_t pm8008_grid_uv(u_int ldo, uint32_t min_uv);

static int fails;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #c); fails++; } } while (0)

static int
always(int rcg, uint32_t hz)
{
	return (1);
}

static int
no_gpll6(int rcg, uint32_t hz)
{
	u_int j;

	for (j = 0; j < camss_rcgs[rcg].nfreq; j++)
		if (camss_rcgs[rcg].freq[j].freq == hz)
			return (camss_src_pll[camss_rcgs[rcg].freq[j].src] !=
			    CAMSS_PLL_GPLL6);
	return (0);
}

int
main(void)
{
	struct camss_rcg_words w;
	uint64_t src[CAMSS_NSRC], hz;
	u_int r, i, s;

	/* every table entry: encode, then decode with the source rate it
	 * implies; all entries of one source must agree on that rate */
	memset(src, 0, sizeof(src));
	src[CAMSS_P_BI_TCXO] = 19200000;
	for (r = 0; r < CAMSS_NRCG; r++) {
		for (i = 0; i < camss_rcgs[r].nfreq; i++) {
			const struct camss_freq *f = &camss_rcgs[r].freq[i];
			uint64_t implied;

			CHECK(camss_rcg_encode(r, f->freq, &w) == 0);
			/* the OPE (not used here) has Linux re-rate GPLL8 for
			 * its entries (CLK_SET_RATE_PARENT): no fixed rate */
			if (f->src == CAMSS_P_GPLL8_OUT_MAIN)
				continue;
			/* freq = src * 2 / (pre_div + 1) * m / n */
			implied = (uint64_t)f->freq * (f->pre_div + 1) / 2;
			if (f->n)
				implied = implied * f->n / f->m;
			if (src[f->src] == 0)
				src[f->src] = implied;
			/* 341333333 * 9 / 4 rounds: within 10 Hz */
			CHECK(implied + 10 >= src[f->src] && implied <= src[f->src] + 10);
		}
	}
	for (s = 0; s < CAMSS_NSRC; s++)
		printf("  source %-16s %llu Hz\n", camss_src_name[s],
		    (unsigned long long)src[s]);
	for (r = 0; r < CAMSS_NRCG; r++) {
		for (i = 0; i < camss_rcgs[r].nfreq; i++) {
			const struct camss_freq *f = &camss_rcgs[r].freq[i];

			if (f->src == CAMSS_P_GPLL8_OUT_MAIN)
				continue;
			camss_rcg_encode(r, f->freq, &w);
			hz = camss_rcg_decode(r, w.cfg, w.m, w.n, src);
			if (hz + 10 < f->freq || hz > f->freq + 10)
				printf("  %s %u: decoded %llu\n", camss_rcgs[r].name,
				    f->freq, (unsigned long long)hz);
			CHECK(hz + 10 >= f->freq && hz <= f->freq + 10);
		}
	}
	/* the words for the rates the camera uses */
	CHECK(camss_rcg_encode(CAMSS_RCG_CAMSS_MCLK0, 19200000, &w) == 0);
	CHECK(w.cfg == 1 && w.mnd == 0);		/* XO, h = 1 */
	CHECK(camss_rcg_encode(CAMSS_RCG_CAMSS_CCI, 37500000, &w) == 0);
	CHECK(w.cfg == (15 | 2 << 8));		/* GPLL0_OUT_AUX2 (2), h = 8 */
	CHECK(camss_rcg_encode(CAMSS_RCG_CAMSS_MCLK2, 24000000, &w) == 0);
	CHECK(w.mnd && w.m == 1 && w.n == (~23u & 0xff) && w.d == (~24u & 0xff));
	CHECK((w.cfg & RCG_CFG_MODE_MASK) == RCG_CFG_MODE_DUAL_EDGE);
	CHECK(camss_rcg_encode(CAMSS_RCG_CAMSS_TFE_0, 153600000, &w) == 0);
	CHECK(w.cfg == (14 | 3 << 8));		/* GPLL10 (3), h = 7.5 */
	CHECK(camss_rcg_encode(CAMSS_RCG_CAMSS_CCI, 37000000, &w) != 0);
	CHECK(src[CAMSS_P_GPLL10_OUT_MAIN] == 0x3c * 19200000ULL);	/* L */

	/* PM8008: the device tree's ranges (sm6115-fxtec-pro1x.dts) */
	CHECK(pm8008_grid_uv(1, 1100000) == 1104000);	/* l1p 1.1-1.2 V */
	CHECK(pm8008_grid_uv(2, 1050000) == 1056000);	/* l2p 1.05-1.056 V */
	CHECK(pm8008_grid_uv(4, 2800000) == 2800000);
	CHECK(pm8008_grid_uv(7, 1800000) == 1800000);
	CHECK(pm8008_grid_uv(5, 2800000) == 2800000);

	/* the CSIPHY's settle count (csiphy_settle_cnt_calc) */
	CHECK(camss_settle_cnt(456800000, 200000000) == 12);
	CHECK(camss_settle_cnt(367000000, 100000000) == 3);
	CHECK(camss_settle_cnt(286400000, 200000000) == 13);
	CHECK(camss_stride(4000, 10) == 5000);
	CHECK(camss_stride(3264, 10) == 4080);
	CHECK(camss_stride(1632, 10) == 2040);
	CHECK(camss_stride(752, 10) == 944);
	{
		uint8_t l[4] = { 0, 1, 2, 3 };
		CHECK(camss_lane_assign(l, 4) == 0x3210);
	}
	/* the stream's clock rates, as camss.c's formulas pick them for the
	 * sensors' modes (link frequency, pixel rate from imx582.c, s5k4h7.c) */
	{
		static const struct { uint64_t link, px; uint32_t cphy, timer,
		    csid, tfe; } m[] = {
			{ 456800000, 576000000, 240000000, 200000000, 192000000, 153600000 },
			{ 286400000, 321600000, 240000000, 100000000, 192000000, 153600000 },
			{ 367000000, 275200000, 240000000, 100000000, 192000000, 153600000 },
			{ 1000000000, 576000000, 341333333, 268800000, 384000000, 153600000 },
		};
		for (i = 0; i < nitems(m); i++) {
			uint64_t min = camss_margin(m[i].link / 4);
			CHECK(camss_pick_rate(CAMSS_RCG_CAMSS_TFE_CPHY_RX,
			    camss_rate_cphy, 3, min, always) == m[i].cphy);
			CHECK(camss_pick_rate(CAMSS_RCG_CAMSS_CSI0PHYTIMER,
			    camss_rate_timer, 3, min, always) == m[i].timer);
			CHECK(camss_pick_rate(CAMSS_RCG_CAMSS_TFE_0_CSID,
			    camss_rate_csid, 4, min, always) == m[i].csid);
			CHECK(camss_pick_rate(CAMSS_RCG_CAMSS_TFE_0, camss_rate_tfe,
			    6, camss_margin(m[i].px * 10 / 64), always) == m[i].tfe);
		}
		/* without GPLL6 the CSID takes GPLL0's 240 MHz */
		CHECK(camss_pick_rate(CAMSS_RCG_CAMSS_TFE_0_CSID, camss_rate_csid,
		    4, camss_margin(456800000 / 4), no_gpll6) == 240000000);
	}
	/* _IOR/_IOWR carry at most IOCPARM_MAX (8191) bytes */
	printf("  sizeof camss_info %zu, camss_sensor %zu\n",
	    sizeof(struct camss_info), sizeof(struct camss_sensor));
	CHECK(sizeof(struct camss_info) < 8192);

	printf("test_calc: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return (fails != 0);
}
