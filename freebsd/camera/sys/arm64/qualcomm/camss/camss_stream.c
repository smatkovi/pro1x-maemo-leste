/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Stage 2 of freebsd/camera/README.md, raw frames: not written yet. The
 * arithmetic the stream will use is here already (host tested).
 */

#include <sys/param.h>
#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/bus.h>
#else
#include <stdint.h>
#ifndef roundup2
#define	roundup2(x, y)	(((x) + ((y) - 1)) & ~((y) - 1))
#endif
#endif

#include <dev/ofw/openfirm.h>

#include "camss_var.h"
#include "camss_stream.h"

/* csiphy_settle_cnt_calc() of camss-csiphy-3ph-1-0.c */
uint8_t
camss_settle_cnt(uint64_t link_freq, uint32_t timer_hz)
{
	uint32_t ui, timer_period, t_hs_settle;

	if (link_freq == 0 || timer_hz == 0)
		return (0);
	ui = (uint32_t)(1000000000000ULL / link_freq);
	ui /= 2;
	t_hs_settle = 85000 + 6 * ui;		/* t_hs_prepare_max */
	timer_period = (uint32_t)(1000000000000ULL / timer_hz);
	return ((uint8_t)(t_hs_settle / timer_period - 6));
}

/* packed RAW (bpp bits a pixel), lines CAMSS_BPL_ALIGN aligned */
uint32_t
camss_stride(uint32_t width, uint32_t bpp)
{
	return (roundup2(width * bpp / 8, CAMSS_BPL_ALIGN));
}

/* csid_get_lane_assign() */
uint32_t
camss_lane_assign(const uint8_t *lanes, u_int n)
{
	uint32_t a = 0;
	u_int i;

	for (i = 0; i < n; i++)
		a |= (uint32_t)lanes[i] << (i * 4);
	return (a);
}

#ifdef _KERNEL
int
camss_stream_attach(struct camss_softc *sc)
{
	return (0);
}

void
camss_stream_detach(struct camss_softc *sc)
{
}

int
camss_stream_start(struct camss_softc *sc, struct camss_stream *s)
{
	return (EOPNOTSUPP);
}

int
camss_stream_stop(struct camss_softc *sc)
{
	return (0);
}

int
camss_stream_qbuf(struct camss_softc *sc, struct camss_buf *b)
{
	return (EOPNOTSUPP);
}

int
camss_stream_dqbuf(struct camss_softc *sc, struct camss_buf *b)
{
	return (EOPNOTSUPP);
}

int
camss_stream_regdump(struct camss_softc *sc, struct camss_regdump *r)
{
	return (EOPNOTSUPP);
}

int
camss_stream_mmap(struct camss_softc *sc, vm_ooffset_t off, vm_paddr_t *pa,
    vm_memattr_t *memattr)
{
	return (EINVAL);
}
#endif
