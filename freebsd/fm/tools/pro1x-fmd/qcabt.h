/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Bluetooth side of the WCN3950 from user space (Linux's hci_qca.c and
 * btqca.c, as the F(x)tec Pro1X's Leste kernel runs them, with its patch
 * 0013): the H4 framing on the UART, Qualcomm's in-band sleep (IBS), HCI
 * commands with their credits, and the chip's bring-up: the power pulses,
 * the version, the baud rate, the rampatch and NVM download, HCI_RESET.
 * The FM receiver's packets (0x11 commands, 0x14 events) travel in the
 * same stream (qcafm.h).
 *
 * Plain C over a small set of OS hooks (struct qbt_ops), single threaded:
 * the daemon (pro1x-fmd.c) runs it on the UART's tty, the host test
 * (freebsd/fm/test) on a pty with a simulated chip.
 */

#ifndef _QCABT_H_
#define	_QCABT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* H4 packet types (hci_qca.c, hci_h4.c) */
#define	H4_CMD			0x01
#define	H4_ACL			0x02
#define	H4_SCO			0x03
#define	H4_EVENT		0x04
#define	H4_ISO			0x05
#define	QCA_FM_CMD		0x11	/* the FM receiver's commands */
#define	QCA_FM_EVT		0x14	/* and events */
#define	IBS_SLEEP_IND		0xfe	/* in-band sleep: one byte each */
#define	IBS_WAKE_IND		0xfd
#define	IBS_WAKE_ACK		0xfc

/* HCI */
#define	HCI_EV_CMD_COMPLETE	0x0e
#define	HCI_EV_CMD_STATUS	0x0f
#define	HCI_EV_HW_ERROR		0x10
#define	HCI_EV_VENDOR		0xff
#define	HCI_OP_RESET		0x0c03

/* Qualcomm's vendor commands (btqca.h) */
#define	EDL_PATCH_CMD_OPCODE	0xfc00
#define	EDL_PATCH_VER_REQ_CMD	0x19
#define	EDL_PATCH_TLV_REQ_CMD	0x1e
#define	EDL_CMD_REQ_RES_EVT	0x00
#define	EDL_APP_VER_RES_EVT	0x02
#define	EDL_TVL_DNLD_RES_EVT	0x04
#define	EDL_TAG_ID_HCI		17
#define	EDL_TAG_ID_DEEP_SLEEP	27
#define	MAX_SIZE_PER_TLV_SEGMENT 243
#define	QCA_SET_BAUDRATE_OPCODE	0xfc48
#define	QCA_PRE_SHUTDOWN_CMD	0xfc08
#define	QCA_POWERON_PULSE	0xfc	/* at 115200 baud */
#define	QCA_POWEROFF_PULSE	0xc0	/* at 2400 baud */

/* btqca.h: the rate codes of the baud rate command and the NVM */
#define	QCA_BAUDRATE_115200	0
#define	QCA_BAUDRATE_3000000	14
#define	QCA_BAUDRATE_3200000	17

/* the TLV files' download modes (btqca.h, enum qca_tlv_dnld_mode) */
#define	QCA_SKIP_EVT_NONE	0
#define	QCA_SKIP_EVT_VSE	1
#define	QCA_SKIP_EVT_CC		2
#define	QCA_SKIP_EVT_VSE_CC	3

/* timeouts as Linux has them */
#define	QBT_CMD_TIMEOUT_MS	2000	/* HCI_CMD_TIMEOUT */
#define	QBT_INIT_TIMEOUT_MS	10000	/* HCI_INIT_TIMEOUT */
#define	QBT_WAKE_RETRANS_MS	100	/* IBS_WAKE_RETRANS_TIMEOUT_MS */
#define	QBT_TX_IDLE_MS		2000	/* IBS_HOST_TX_IDLE_TIMEOUT_MS */
#define	QBT_WAKE_TRIES		10

#define	QBT_FRAME_MAX		(4 + 1024)

struct qbt;

struct qbt_ops {
	/* all of buf, or -errno */
	int	(*write)(void *arg, const void *buf, size_t len);
	/* up to len bytes within timeout_ms: >0 bytes, 0 none, -errno */
	int	(*read)(void *arg, void *buf, size_t len, int timeout_ms);
	int	(*set_speed)(void *arg, unsigned int baud);
	/*
	 * Hardware flow control on (RTS/CTS), or off with RTS deasserted, as
	 * hci_uart_set_flow_control() does it (its "enable" means "stop").
	 */
	int	(*set_flow)(void *arg, bool on);
	int	(*drain)(void *arg);		/* until written out */
	void	(*flush_input)(void *arg);
	uint64_t (*now_ms)(void *arg);
	void	(*sleep_ms)(void *arg, unsigned int ms);
	void	(*log)(void *arg, const char *msg);
	/* a firmware file, malloc'd: 0 or -errno */
	int	(*fw_load)(void *arg, const char *name, uint8_t **data,
		    size_t *len);
};

/* An FM event (code, parameters) or other unsolicited packet */
typedef void qbt_fm_cb_t(void *arg, uint8_t evt, const uint8_t *p,
    uint8_t plen);

struct qbt_version {
	uint32_t	product_id;
	uint16_t	patch_ver;
	uint16_t	rom_ver;
	uint32_t	soc_id;
};

enum qbt_tx_state { IBS_TX_ASLEEP, IBS_TX_WAKING, IBS_TX_AWAKE };

struct qbt {
	const struct qbt_ops *ops;
	void		*arg;
	unsigned int	oper_speed;	/* the speed after the version */
	int		debug;		/* 1: frames, 2: and IBS */

	/* the H4 receiver */
	uint8_t		rx[QBT_FRAME_MAX];
	size_t		rx_len, rx_need;
	unsigned int	rx_errors;	/* unknown types, oversized */

	/* in-band sleep */
	bool		ibs;		/* off during the download */
	enum qbt_tx_state tx_state;
	bool		rx_awake;
	uint64_t	last_tx;
	unsigned int	ibs_sent_wakes, ibs_sent_slps, ibs_sent_wacks;
	unsigned int	ibs_recv_wakes, ibs_recv_slps, ibs_recv_wacks;

	/* HCI commands: one at a time */
	int		credits;
	uint16_t	wait_op;	/* the command waited for */
	uint8_t		wait_ev;	/* its completion: CC, or VSE */
	bool		done;
	int		status;		/* the completion's status, -1 none */
	uint8_t		rsp[260];
	size_t		rsp_len;
	unsigned int	cmd_timeouts;	/* no credit within 2 s */

	/* the FM receiver's packets: one command at a time as well */
	uint16_t	fm_wait_op;
	bool		fm_done;
	int		fm_status;
	uint8_t		fm_rsp[260];
	size_t		fm_rsp_len;
	qbt_fm_cb_t	*fm_cb;
	void		*fm_arg;

	/* the chip */
	bool		up;		/* firmware loaded, reset done */
	struct qbt_version ver;
	uint8_t		rom;		/* the files' ROM version */
	char		fw_rampatch[64], fw_nvm[64];
};

void	qbt_init(struct qbt *bt, const struct qbt_ops *ops, void *arg,
	    unsigned int oper_speed);
void	qbt_logf(struct qbt *bt, const char *fmt, ...)
	    __attribute__((format(printf, 2, 3)));

/* Bytes from the UART: frames dispatched (IBS answered, events stored) */
void	qbt_input(struct qbt *bt, const uint8_t *buf, size_t len);
/*
 * Reads and dispatches what arrives within timeout_ms (0: what is there).
 * 0, or -errno of the read.
 */
int	qbt_pump(struct qbt *bt, int timeout_ms);
/* Timers: the IBS sleep after the transmit idle time. Returns ms to wait. */
int	qbt_tick(struct qbt *bt);

/* A frame (type byte first) through in-band sleep. 0 or -errno */
int	qbt_send(struct qbt *bt, const uint8_t *frame, size_t len);

/*
 * An HCI command; waits for its Command Complete (wait_ev 0), or for a
 * vendor event (HCI_EV_VENDOR), or not at all (raw: wait_ev 1, no
 * credit). The completion's parameters in bt->rsp (CC: from the status
 * on; VSE: all). 0, -ETIMEDOUT, -errno.
 */
#define	QBT_WAIT_CC	0
#define	QBT_NO_WAIT	1
int	qbt_cmd(struct qbt *bt, uint16_t opcode, const void *param,
	    uint8_t plen, uint8_t wait_ev, int timeout_ms);

/* An FM command, to its Command Complete or Status (rsp after status) */
int	qbt_fm_cmd(struct qbt *bt, uint16_t opcode, const void *param,
	    uint8_t plen, void *rsp, size_t rsp_len, int timeout_ms);

/*
 * The chip from cold to set up (qca_setup), up to tries times. The
 * firmware file names come from the version (qca_uart_setup).
 */
int	qbt_power_on(struct qbt *bt, int tries);
/* The pre-shutdown command; IBS off (qca_hci_shutdown) */
int	qbt_power_off(struct qbt *bt);

/* the steps, for the tests */
int	qbt_read_version(struct qbt *bt, struct qbt_version *ver);
int	qbt_set_baudrate(struct qbt *bt, unsigned int speed);
uint8_t	qbt_baud_code(unsigned int speed);
uint8_t	qbt_rom_version(const struct qbt_version *ver);
/*
 * The checks and changes of qca_tlv_check_data: the rampatch's download
 * mode (*mode), the NVM's tags (HCI: in-band sleep on, the rate; deep
 * sleep on). type: 1 rampatch, 2 NVM. 0 or -EINVAL.
 */
int	qbt_tlv_check(uint8_t *data, size_t len, int type, uint8_t baud,
	    int *mode);
int	qbt_download(struct qbt *bt, const char *name, int type);

#endif /* _QCABT_H_ */
