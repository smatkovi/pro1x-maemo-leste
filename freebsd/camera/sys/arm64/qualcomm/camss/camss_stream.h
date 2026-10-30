/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The CAMSS driver's state (qcom_camss.c) and its raw frame path
 * (camss_stream.c).
 */
#ifndef _CAMSS_STREAM_H_
#define	_CAMSS_STREAM_H_

#include "qcom_camss_ioctl.h"

/* the arithmetic shared with the host tests (camss_stream.c) */
#define	CAMSS_CLOCK_MARGIN_NUM	105	/* camss.c CAMSS_CLOCK_MARGIN_* */
#define	CAMSS_CLOCK_MARGIN_DEN	100
#define	CAMSS_BPL_ALIGN		8	/* vfe_bpl_align(): CAMSS_2290 */
uint8_t	camss_settle_cnt(uint64_t link_freq, uint32_t timer_hz);
uint32_t camss_stride(uint32_t width, uint32_t bpp);
uint32_t camss_lane_assign(const uint8_t *lanes, u_int n);
extern const uint32_t camss_rate_cphy[3], camss_rate_timer[3];
extern const uint32_t camss_rate_csid[4], camss_rate_tfe[6];
uint64_t camss_margin(uint64_t hz);
uint32_t camss_pick_rate(int rcg, const uint32_t *tbl, u_int n, uint64_t min,
	    int (*usable)(int, uint32_t));

#ifdef _KERNEL
#include <sys/lock.h>
#include <sys/sx.h>

struct camss_sensor_priv {
	phandle_t	node;
	int		mclk_branch;
	int		mclk_on;
};

enum camss_bufstate { BUF_USER, BUF_QUEUED, BUF_HW, BUF_DONE };

struct camss_dmabuf {
	void		*va;
	vm_paddr_t	pa;
	enum camss_bufstate state;
	uint32_t	sequence;
	uint32_t	flags;
	uint64_t	ts;
	uint32_t	tfe_status, bus_status;
};

struct camss_softc {
	device_t	dev;
	phandle_t	node;
	struct sx	lock;
	struct cdev	*cdev;
	struct camss_info info;
	struct camss_sensor_priv sensor[CAMSS_MAX_SENSORS];

	/* stage 2 */
	struct resource	*csiphy, *csid, *vfe;
	int		csiphy_rid, csid_rid, vfe_rid;
	int		streaming;
	u_int		port;
	struct camss_stream cfg;
	int		clk[12];
	int		nclk;
	int		gdsc;
	u_int		nbufs;
	uint32_t	bufsize;
	struct camss_dmabuf buf[CAMSS_MAX_BUFS];
	int		hw[2];		/* buffers given to the write master */
	uint32_t	sequence;
	uint32_t	reg_update;
	uint32_t	overflow, violation;
	u_int		frames, errors;
};

int	camss_stream_attach(struct camss_softc *sc);
void	camss_stream_detach(struct camss_softc *sc);
int	camss_stream_start(struct camss_softc *sc, struct camss_stream *s);
int	camss_stream_stop(struct camss_softc *sc);
int	camss_stream_qbuf(struct camss_softc *sc, struct camss_buf *b);
int	camss_stream_dqbuf(struct camss_softc *sc, struct camss_buf *b);
int	camss_stream_regdump(struct camss_softc *sc, struct camss_regdump *r);
int	camss_stream_mmap(struct camss_softc *sc, vm_ooffset_t off,
	    vm_paddr_t *pa, vm_memattr_t *memattr);
#endif

#endif /* _CAMSS_STREAM_H_ */
