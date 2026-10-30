/*
 * Host test of dwc3dci_qmp.h: the SM6115 QMP USB3 PHY tables generated
 * from Linux 7.1's phy-qcom-qmp-usbc.c (qcm2290_usb3_*_tbl) against
 * entries read from Linux's source and headers by hand.
 *
 *   cc -I freebsd/pm/sys -o /tmp/qmpt freebsd/pm/test/qmp_test.c && /tmp/qmpt
 */
#include <stdint.h>
#include <stdio.h>
#include "dev/usb/controller/dwc3dci_qmp.h"

#define	N(t) (sizeof(t) / sizeof((t)[0]))
static int fails;
#define	EQ(a, b) do { long _a = (a), _b = (b); if (_a != _b) { \
	printf("FAIL %d: %s = %#lx, want %#lx\n", __LINE__, #a, _a, _b); fails++; } } while (0)

static int
find(const struct qmp_init *t, size_t n, unsigned off)
{
	for (size_t i = 0; i < n; i++)
		if (t[i].off == off)
			return (t[i].val);
	return (-1);
}

static void
unique(const struct qmp_init *t, size_t n, int line)
{
	for (size_t i = 0; i < n; i++)
		for (size_t j = i + 1; j < n; j++)
			if (t[i].off == t[j].off) {
				printf("FAIL %d: offset %#x twice\n", line, t[i].off);
				fails++;
			}
}

int
main(void)
{
	/* QSERDES_COM_SYSCLK_EN_SEL 0xac = 0x14, CLK_SELECT 0x174 = 0x30 */
	EQ(find(qmp_serdes_tbl, N(qmp_serdes_tbl), 0x0ac), 0x14);
	EQ(find(qmp_serdes_tbl, N(qmp_serdes_tbl), 0x174), 0x30);
	EQ(qmp_serdes_tbl[0].off, 0x0ac);
	unique(qmp_serdes_tbl, N(qmp_serdes_tbl), __LINE__);
	unique(qmp_tx_tbl, N(qmp_tx_tbl), __LINE__);
	unique(qmp_rx_tbl, N(qmp_rx_tbl), __LINE__);
	unique(qmp_pcs_tbl, N(qmp_pcs_tbl), __LINE__);
	/* the blocks fit their windows (0x200 each, PCS 0x400) */
	for (size_t i = 0; i < N(qmp_tx_tbl); i++)
		if (qmp_tx_tbl[i].off >= 0x200) { fails++; printf("FAIL tx %#x\n", qmp_tx_tbl[i].off); }
	for (size_t i = 0; i < N(qmp_rx_tbl); i++)
		if (qmp_rx_tbl[i].off >= 0x200) { fails++; printf("FAIL rx %#x\n", qmp_rx_tbl[i].off); }
	for (size_t i = 0; i < N(qmp_pcs_tbl); i++)
		if (qmp_pcs_tbl[i].off >= 0x400) { fails++; printf("FAIL pcs %#x\n", qmp_pcs_tbl[i].off); }
	printf("tables: serdes %zu, tx %zu, rx %zu, pcs %zu\n", N(qmp_serdes_tbl),
	    N(qmp_tx_tbl), N(qmp_rx_tbl), N(qmp_pcs_tbl));
	printf("%s: %d failures\n", fails ? "FAILED" : "OK", fails);
	return (fails != 0);
}
