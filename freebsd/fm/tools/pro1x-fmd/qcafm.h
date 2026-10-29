/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The WCN3950's FM receiver over the Bluetooth UART (qcabt.h): what the
 * Pro1X's Linux driver radio-qca-fm.c (device/kernel/patches/0015, 0017)
 * does, without V4L2. The commands are Qualcomm's FM HCI set (radio-iris,
 * the helium FM HAL of fm-commonsys): enable, the receiver's configuration
 * (87.5-108 MHz on a 100 kHz grid, RDS, de-emphasis), stereo or mono,
 * mute, tune (kHz), the station's parameters (RSSI, stereo, RDS sync),
 * seek, the chip's own RDS decoding (station name, radio text), and the
 * SLIMbus data port for the audio (FM_ENABLE_SLIMBUS).
 */

#ifndef _QCAFM_H_
#define	_QCAFM_H_

#include <stdbool.h>
#include <stdint.h>

#include <errno.h>

#include "qcabt.h"

/* a seek that found nothing (V4L2's ENODATA, which FreeBSD lacks) */
#ifdef ENODATA
#define	QFM_ENOSTATION		ENODATA
#else
#define	QFM_ENOSTATION		ENOMSG
#endif

#define	FM_OP(ogf, ocf)		((uint16_t)((ogf) << 10 | (ocf)))
#define	FM_RX(ocf)		FM_OP(0x13, ocf)
#define	FM_COMMON(ocf)		FM_OP(0x15, ocf)
#define	FM_DIAG(ocf)		FM_OP(0x3f, ocf)

#define	FM_RX_ENABLE		FM_RX(0x01)
#define	FM_RX_DISABLE		FM_RX(0x02)
#define	FM_RX_SET_CONF		FM_RX(0x04)
#define	FM_RX_SET_MUTE		FM_RX(0x05)
#define	FM_RX_SET_STEREO	FM_RX(0x06)
#define	FM_RX_GET_STATION	FM_RX(0x0a)
#define	FM_RX_SEARCH		FM_RX(0x0e)
#define	FM_RX_CANCEL_SEARCH	FM_RX(0x11)
#define	FM_RX_RDS_PROCESS	FM_RX(0x13)
#define	FM_RX_EVENT_MASK	FM_RX(0x16)
#define	FM_TUNE			FM_COMMON(0x01)
#define	FM_ENABLE_SLIMBUS	FM_DIAG(0x0e)

#define	FM_EV_TUNE_STATUS	0x01
#define	FM_EV_PS		0x09
#define	FM_EV_RT		0x0a
#define	FM_EV_SEARCH_COMPLETE	0x12
#define	FM_EV_SEARCH_RDS_COMPLETE 0x13

#define	FM_BAND_LOW		87500	/* kHz */
#define	FM_BAND_HIGH		108000
#define	FM_CMD_TIMEOUT_MS	2000
#define	FM_TUNE_TIMEOUT_MS	2000
#define	FM_SEEK_TIMEOUT_MS	30000

/* the answer to FM_RX_GET_STATION after its status, and the tune status */
struct fm_station {
	uint32_t	freq;		/* kHz */
	uint8_t		service;
	int8_t		rssi;		/* dBm */
	uint8_t		stereo;
	uint8_t		rds_sync;
	uint8_t		mute;
	int8_t		sinr;
	uint8_t		intf_det_th;
};

struct qfm {
	struct qbt	*bt;
	bool		on;
	uint32_t	freq;		/* kHz */
	bool		mute, mono, emphasis75;
	bool		slimbus;	/* FM_ENABLE_SLIMBUS answered */
	/* the last station parameters and events */
	struct fm_station st;
	unsigned int	tunes;		/* tune status events */
	bool		seek_done;
	/* RDS: the chip's decoding */
	uint8_t		pty;
	char		ps[9], rt[65];
	unsigned int	ps_count, rt_count;
	void		(*changed)(void *arg, const char *what);
	void		*changed_arg;
};

void	qfm_init(struct qfm *fm, struct qbt *bt);
/* the FM events of qcabt (its fm_cb) */
void	qfm_event(void *arg, uint8_t evt, const uint8_t *p, uint8_t plen);

/* radio-qca-fm.c: qca_fm_power_on / _off, with the controller up */
int	qfm_on(struct qfm *fm);
int	qfm_off(struct qfm *fm);
/* on the band's 100 kHz grid */
uint32_t qfm_round(uint32_t khz);
int	qfm_tune(struct qfm *fm, uint32_t khz);
/* -QFM_ENOSTATION: no station, the receiver back where it started */
int	qfm_seek(struct qfm *fm, bool up);
int	qfm_station(struct qfm *fm, struct fm_station *st);
int	qfm_set_mute(struct qfm *fm, bool mute);
int	qfm_set_mono(struct qfm *fm, bool mono);
/* the signal as radio-qca-fm reports it: -120 dBm 0 %, -80 dBm 100 % */
int	qfm_signal(int rssi);
/* the parsers, for the tests */
void	qfm_parse_station(const uint8_t *p, struct fm_station *st);

#endif /* _QCAFM_H_ */
