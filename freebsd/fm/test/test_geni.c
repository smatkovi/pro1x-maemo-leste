/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The GENI UART's arithmetic (sys/dev/uart/uart_qcom_geni_calc.h) on the
 * PC: the clock levels Linux would pick for the bring-up's rates, the
 * RCG registers for them, the RCG rate read back, the FIFO packing.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../sys/dev/uart/uart_qcom_geni_calc.h"

static int failures;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); \
	failures++; } } while (0)

int
main(void)
{
	static const uint32_t bauds[] = { 2400, 115200, 921600, 3000000 };
	/* QUP v2.5 and later sample 16 times, before 32 times */
	static const uint32_t versions[] = { 0x20050000, 0x10000000 };
	struct geni_rcg_regs r;
	unsigned int idx, v, b;
	uint32_t div, cfg0, cfg1;
	uint64_t rate;

	for (v = 0; v < 2; v++)
		for (b = 0; b < 4; b++) {
			CHECK(geni_uart_clk_match(bauds[b], versions[v], &idx,
			    &div) == 0);
			rate = geni_clk_table[idx].rate / div;
			printf("QUP %#x, %7u baud: %9u Hz (level %u) / %4u = "
			    "%llu Hz (%.3f %%)\n", versions[v], bauds[b],
			    geni_clk_table[idx].rate, idx, div,
			    (unsigned long long)rate, 100.0 * ((double)rate /
			    ((double)bauds[b] * (v == 0 ? 16 : 32)) - 1));
			/* exact for these: Linux's first exact match */
			CHECK(rate == (uint64_t)bauds[b] * (v == 0 ? 16 : 32));
		}
	/* 3 Mbaud: 48 MHz from GPLL0_OUT_AUX2 * 4/25 at v2.5 */
	CHECK(geni_uart_clk_match(3000000, 0x20050000, &idx, &div) == 0 &&
	    idx == 5 && div == 1);
	geni_rcg_regs(&geni_clk_table[idx], &r);
	printf("48 MHz: M %#x N %#x D %#x CFG %#x/%#x\n", r.m, r.n, r.d,
	    r.cfg, r.cfg_mask);
	CHECK(r.mnd && r.m == 4 && r.n == (~21u & 0xffff) &&
	    r.d == (~25u & 0xffff) && r.cfg == (0x2201u | 0) + 0x0000);
	CHECK(geni_rcg_rate(r.cfg, r.m, r.n) == 48000000);
	/* 7.3728 MHz (115200 and 2400 baud) */
	geni_rcg_regs(&geni_clk_table[0], &r);
	CHECK(r.m == 384 && r.n == (~(15625u - 384) & 0xffff) &&
	    r.d == (~15625u & 0xffff) && r.cfg == 0x2201);
	CHECK(geni_rcg_rate(r.cfg, r.m, r.n) == 7372800);
	/* 19.2 MHz from the TCXO: no M/N, no dual edge */
	geni_rcg_regs(&geni_clk_table[2], &r);
	CHECK(!r.mnd && r.cfg == 0x0001);
	CHECK(geni_rcg_rate(r.cfg, 0, 0) == 19200000);
	/* 75 MHz: AUX2 / 4 (pre_div 7) */
	geni_rcg_regs(&geni_clk_table[7], &r);
	CHECK(geni_rcg_rate(r.cfg, 0, 0) == 75000000);
	/* too slow for the 12-bit divider */
	CHECK(geni_uart_clk_match(300, 0x20050000, &idx, &div) == -1 ||
	    div <= GENI_CLK_DIV_MAX);
	/* geni_se_config_packing(8, 4, false) */
	geni_packing(8, 4, false, &cfg0, &cfg1);
	printf("packing 8x4: %#x %#x\n", cfg0, cfg1);
	CHECK(cfg0 == 0x4380e && cfg1 == 0xc3e0e);
	printf("%s: %d failures\n", failures ? "FAIL" : "ok", failures);
	return (failures != 0);
}
