/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The WCN3950's Bluetooth side from user space: what Linux's hci_qca.c and
 * btqca.c do for it (qcabt.h). Comments name the Linux functions each part
 * follows; the Pro1X's Leste kernel (7.1.0-pro1x.4 and later) brings the
 * chip up this way, with patch 0013 (the NVM's rate in byte 1 of tag 17).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcabt.h"

#define	QCA_WCN3950_SOC_ID_T	0x40074130
#define	QCA_WCN3950_SOC_ID_S	0x40075130
#define	TLV_HDR			4	/* struct tlv_type_hdr */
#define	TLV_PATCH		24	/* struct tlv_type_patch */
#define	TLV_NVM			12	/* struct tlv_type_nvm */

static uint16_t
le16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
le32(const uint8_t *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

void
qbt_logf(struct qbt *bt, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	bt->ops->log(bt->arg, buf);
}

static void
log_hex(struct qbt *bt, const char *what, const uint8_t *p, size_t len)
{
	char buf[3 * 48 + 8];
	size_t i, n = len < 48 ? len : 48;

	for (i = 0; i < n; i++)
		snprintf(buf + 3 * i, 4, " %02x", p[i]);
	if (n < len)
		strcpy(buf + 3 * n, " ...");
	qbt_logf(bt, "%s (%zu):%s", what, len, buf);
}

static uint64_t
now(struct qbt *bt)
{
	return (bt->ops->now_ms(bt->arg));
}

void
qbt_init(struct qbt *bt, const struct qbt_ops *ops, void *arg,
    unsigned int oper_speed)
{
	memset(bt, 0, sizeof(*bt));
	bt->ops = ops;
	bt->arg = arg;
	bt->oper_speed = oper_speed;
	bt->credits = 1;
	bt->status = -1;
	bt->tx_state = IBS_TX_ASLEEP;
}

static int
raw_write(struct qbt *bt, const uint8_t *p, size_t len)
{
	if (bt->debug >= 1 && !(len == 1 && p[0] >= IBS_WAKE_ACK))
		log_hex(bt, "tx", p, len);
	else if (bt->debug >= 2)
		qbt_logf(bt, "tx ibs %02x", p[0]);
	return (bt->ops->write(bt->arg, p, len));
}

/* --- receiving ----------------------------------------------------------- */

/* hci_qca.c: device_want_to_wakeup, device_want_to_sleep, device_woke_up */
static void
ibs_byte(struct qbt *bt, uint8_t c)
{
	uint8_t ack = IBS_WAKE_ACK;

	if (bt->debug >= 2)
		qbt_logf(bt, "rx ibs %02x", c);
	switch (c) {
	case IBS_WAKE_IND:
		/* always acknowledged, also while asleep */
		bt->ibs_recv_wakes++;
		bt->rx_awake = true;
		if (raw_write(bt, &ack, 1) == 0)
			bt->ibs_sent_wacks++;
		break;
	case IBS_SLEEP_IND:
		bt->ibs_recv_slps++;
		bt->rx_awake = false;
		break;
	case IBS_WAKE_ACK:
		bt->ibs_recv_wacks++;
		if (bt->tx_state == IBS_TX_WAKING) {
			bt->tx_state = IBS_TX_AWAKE;
			bt->last_tx = now(bt);
		}
		break;
	}
}

static void
hci_event(struct qbt *bt, const uint8_t *e, size_t len)
{
	uint8_t evt = e[0], plen = e[1];
	const uint8_t *p = e + 2;
	uint16_t op;

	(void)len;
	switch (evt) {
	case HCI_EV_CMD_COMPLETE:
		/* ncmd, opcode, then the return parameters (status first) */
		if (plen < 3)
			break;
		bt->credits = p[0];
		op = le16(p + 1);
		if (bt->wait_op == op && bt->wait_ev == QBT_WAIT_CC) {
			bt->done = true;
			bt->status = plen > 3 ? p[3] : 0;
			bt->rsp_len = plen - 3;
			memcpy(bt->rsp, p + 3, bt->rsp_len);
		}
		return;
	case HCI_EV_CMD_STATUS:
		/* status, ncmd, opcode */
		if (plen < 4)
			break;
		bt->credits = p[1];
		op = le16(p + 2);
		if (bt->wait_op == op && p[0] != 0) {
			bt->done = true;
			bt->status = p[0];
			bt->rsp_len = 0;
		}
		return;
	case HCI_EV_VENDOR:
		if (bt->wait_op != 0 && bt->wait_ev == HCI_EV_VENDOR) {
			bt->done = true;
			bt->status = 0;
			bt->rsp_len = plen;
			memcpy(bt->rsp, p, plen);
			return;
		}
		break;
	case HCI_EV_HW_ERROR:
		qbt_logf(bt, "controller hardware error %#04x",
		    plen ? p[0] : 0);
		return;
	}
	if (bt->debug >= 1)
		qbt_logf(bt, "event %#04x (%u bytes) not waited for", evt, plen);
}

/* radio-qca-fm.c: qca_fm_recv's Command Complete and Status */
static void
fm_event(struct qbt *bt, const uint8_t *e, size_t len)
{
	uint8_t evt = e[0], plen = e[1];
	const uint8_t *p = e + 2;
	uint16_t op;

	(void)len;
	if (evt == 0x0f && plen >= 4) {
		/* credits, opcode, status, the answer */
		op = le16(p + 1);
		if (op == bt->fm_wait_op) {
			bt->fm_done = true;
			bt->fm_status = p[3];
			bt->fm_rsp_len = plen - 4;
			memcpy(bt->fm_rsp, p + 4, bt->fm_rsp_len);
		}
		return;
	}
	if (evt == 0x10 && plen >= 4) {
		/* status, credits, opcode */
		op = le16(p + 2);
		if (op == bt->fm_wait_op) {
			bt->fm_done = true;
			bt->fm_status = p[0];
			bt->fm_rsp_len = 0;
		}
		return;
	}
	if (bt->fm_cb != NULL)
		bt->fm_cb(bt->fm_arg, evt, p, plen);
}

static void
frame(struct qbt *bt, const uint8_t *f, size_t len)
{
	if (bt->debug >= 1)
		log_hex(bt, "rx", f, len);
	switch (f[0]) {
	case H4_EVENT:
		hci_event(bt, f + 1, len - 1);
		break;
	case QCA_FM_EVT:
		fm_event(bt, f + 1, len - 1);
		break;
	default:
		/* ACL, SCO, ISO: no Bluetooth user here */
		break;
	}
}

/*
 * H4 framing (hci_h4.c h4_recv_buf with hci_qca's packet table, the FM
 * events of patch 0014 included). An unknown type byte is dropped and the
 * next byte tried; h4_recv_buf drops the whole buffer instead.
 */
void
qbt_input(struct qbt *bt, const uint8_t *buf, size_t len)
{
	size_t i, total;
	uint8_t c;

	for (i = 0; i < len; i++) {
		c = buf[i];
		if (bt->rx_len == 0) {
			switch (c) {
			case IBS_WAKE_IND:
			case IBS_SLEEP_IND:
			case IBS_WAKE_ACK:
				ibs_byte(bt, c);
				continue;
			case H4_EVENT:
			case QCA_FM_EVT:
				bt->rx_need = 3;	/* code, length */
				break;
			case H4_SCO:
				bt->rx_need = 4;	/* handle, length */
				break;
			case H4_ACL:
			case H4_ISO:
				bt->rx_need = 5;	/* handle, 16-bit length */
				break;
			default:
				bt->rx_errors++;
				continue;
			}
			bt->rx[bt->rx_len++] = c;
			continue;
		}
		bt->rx[bt->rx_len++] = c;
		if (bt->rx_len < bt->rx_need)
			continue;
		if (bt->rx_len == 3 && (bt->rx[0] == H4_EVENT ||
		    bt->rx[0] == QCA_FM_EVT))
			total = 3 + bt->rx[2];
		else if (bt->rx_len == 4 && bt->rx[0] == H4_SCO)
			total = 4 + bt->rx[3];
		else if (bt->rx_len == 5 && bt->rx[0] == H4_ACL)
			total = 5 + le16(bt->rx + 3);
		else if (bt->rx_len == 5 && bt->rx[0] == H4_ISO)
			total = 5 + (le16(bt->rx + 3) & 0x3fff);
		else
			total = bt->rx_len;
		if (total > QBT_FRAME_MAX) {
			/* not ours to carry: lose sync, as h4_recv_buf */
			bt->rx_errors++;
			bt->rx_len = 0;
			continue;
		}
		if (total > bt->rx_len) {
			bt->rx_need = total;
			continue;
		}
		frame(bt, bt->rx, bt->rx_len);
		bt->rx_len = 0;
	}
}

int
qbt_pump(struct qbt *bt, int timeout_ms)
{
	uint8_t buf[512];
	int n;

	n = bt->ops->read(bt->arg, buf, sizeof(buf), timeout_ms);
	if (n < 0)
		return (n);
	if (n > 0)
		qbt_input(bt, buf, (size_t)n);
	return (0);
}

/* --- sending: in-band sleep (qca_enqueue, qca_wq_awake_device) ---------- */

int
qbt_tick(struct qbt *bt)
{
	uint64_t t = now(bt);
	uint8_t c = IBS_SLEEP_IND;

	if (!bt->ibs || bt->tx_state != IBS_TX_AWAKE)
		return (-1);
	/* hci_ibs_tx_idle_timeout */
	if (t - bt->last_tx >= QBT_TX_IDLE_MS) {
		if (raw_write(bt, &c, 1) == 0)
			bt->ibs_sent_slps++;
		bt->tx_state = IBS_TX_ASLEEP;
		return (-1);
	}
	return ((int)(bt->last_tx + QBT_TX_IDLE_MS - t));
}

int
qbt_send(struct qbt *bt, const uint8_t *f, size_t len)
{
	uint8_t c = IBS_WAKE_IND;
	uint64_t until;
	int error, i;

	if (bt->ibs && bt->tx_state != IBS_TX_AWAKE) {
		/* wake the controller first; WAKE again every 100 ms */
		bt->tx_state = IBS_TX_WAKING;
		for (i = 0; i < QBT_WAKE_TRIES &&
		    bt->tx_state == IBS_TX_WAKING; i++) {
			if ((error = raw_write(bt, &c, 1)) != 0)
				return (error);
			bt->ibs_sent_wakes++;
			until = now(bt) + QBT_WAKE_RETRANS_MS;
			while (bt->tx_state == IBS_TX_WAKING &&
			    now(bt) < until)
				if ((error = qbt_pump(bt,
				    (int)(until - now(bt)))) != 0)
					return (error);
		}
		if (bt->tx_state != IBS_TX_AWAKE) {
			bt->tx_state = IBS_TX_ASLEEP;
			qbt_logf(bt, "no HCI_IBS_WAKE_ACK after %d wakes", i);
			return (-ETIMEDOUT);
		}
	}
	if ((error = raw_write(bt, f, len)) != 0)
		return (error);
	bt->last_tx = now(bt);
	return (0);
}

/* --- commands ------------------------------------------------------------ */

static int
wait_for(struct qbt *bt, bool *flag, int timeout_ms)
{
	uint64_t until = now(bt) + (uint64_t)timeout_ms;
	int error;

	while (!*flag) {
		if (now(bt) >= until)
			return (-ETIMEDOUT);
		if ((error = qbt_pump(bt, (int)(until - now(bt)))) != 0)
			return (error);
	}
	return (0);
}

int
qbt_cmd(struct qbt *bt, uint16_t opcode, const void *param, uint8_t plen,
    uint8_t wait_ev, int timeout_ms)
{
	uint8_t f[4 + 255];
	uint64_t until;
	int error;

	f[0] = H4_CMD;
	f[1] = opcode & 0xff;
	f[2] = opcode >> 8;
	f[3] = plen;
	if (plen > 0)
		memcpy(f + 4, param, plen);
	if (wait_ev == QBT_NO_WAIT)
		/* __hci_cmd_send: no credit, no answer */
		return (qbt_send(bt, f, 4 + (size_t)plen));

	/* a command credit, else the core's command timeout gives one */
	until = now(bt) + QBT_CMD_TIMEOUT_MS;
	while (bt->credits <= 0) {
		if (now(bt) >= until) {
			qbt_logf(bt, "command %#06x: no credit, sent anyway",
			    opcode);
			bt->cmd_timeouts++;
			bt->credits = 1;
			break;
		}
		if ((error = qbt_pump(bt, (int)(until - now(bt)))) != 0)
			return (error);
	}
	bt->credits--;
	bt->wait_op = opcode;
	bt->wait_ev = wait_ev;
	bt->done = false;
	bt->status = -1;
	bt->rsp_len = 0;
	error = qbt_send(bt, f, 4 + (size_t)plen);
	if (error == 0)
		error = wait_for(bt, &bt->done, timeout_ms);
	bt->wait_op = 0;
	if (error == -ETIMEDOUT)
		qbt_logf(bt, "command %#06x: no answer in %d ms", opcode,
		    timeout_ms);
	return (error);
}

int
qbt_fm_cmd(struct qbt *bt, uint16_t opcode, const void *param, uint8_t plen,
    void *rsp, size_t rsp_len, int timeout_ms)
{
	uint8_t f[4 + 255];
	int error;

	if (!bt->up)
		return (-ENETDOWN);
	f[0] = QCA_FM_CMD;
	f[1] = opcode & 0xff;
	f[2] = opcode >> 8;
	f[3] = plen;
	if (plen > 0)
		memcpy(f + 4, param, plen);
	bt->fm_wait_op = opcode;
	bt->fm_done = false;
	bt->fm_status = -1;
	bt->fm_rsp_len = 0;
	error = qbt_send(bt, f, 4 + (size_t)plen);
	if (error == 0)
		error = wait_for(bt, &bt->fm_done, timeout_ms);
	bt->fm_wait_op = 0;
	if (error != 0) {
		qbt_logf(bt, "FM command %#06x: %s", opcode, strerror(-error));
		return (error);
	}
	if (bt->fm_status != 0) {
		qbt_logf(bt, "FM command %#06x: status %#04x", opcode,
		    bt->fm_status);
		return (-EIO);
	}
	if (rsp != NULL) {
		if (bt->fm_rsp_len < rsp_len)
			return (-EPROTO);
		memcpy(rsp, bt->fm_rsp, rsp_len);
	}
	return (0);
}

/* --- the bring-up --------------------------------------------------------- */

/* qca_get_baudrate_value */
uint8_t
qbt_baud_code(unsigned int speed)
{
	switch (speed) {
	case 9600:	return (4);
	case 19200:	return (3);
	case 38400:	return (2);
	case 57600:	return (1);
	case 115200:	return (QCA_BAUDRATE_115200);
	case 230400:	return (5);
	case 460800:	return (7);
	case 500000:	return (8);
	case 921600:	return (10);
	case 1000000:	return (11);
	case 2000000:	return (13);
	case 3000000:	return (QCA_BAUDRATE_3000000);
	case 3200000:	return (QCA_BAUDRATE_3200000);
	case 3500000:	return (18);
	default:	return (QCA_BAUDRATE_115200);
	}
}

/* qca_send_power_pulse: one byte with flow control off, RTS deasserted */
static int
power_pulse(struct qbt *bt, bool on)
{
	uint8_t c = on ? QCA_POWERON_PULSE : QCA_POWEROFF_PULSE;
	int error;

	bt->ops->set_flow(bt->arg, false);
	error = raw_write(bt, &c, 1);
	bt->ops->drain(bt->arg);
	bt->ops->set_flow(bt->arg, true);
	if (error != 0)
		return (error);
	/* time to boot or to shut down */
	bt->ops->sleep_ms(bt->arg, on ? 100 : 10);
	return (0);
}

/* qca_read_soc_version: the answer comes in a vendor event (< WCN3991) */
int
qbt_read_version(struct qbt *bt, struct qbt_version *ver)
{
	uint8_t cmd = EDL_PATCH_VER_REQ_CMD;
	int error;

	error = qbt_cmd(bt, EDL_PATCH_CMD_OPCODE, &cmd, 1, HCI_EV_VENDOR,
	    3000);
	if (error != 0) {
		qbt_logf(bt, "Reading QCA version information failed (%d)",
		    error);
		return (error);
	}
	if (bt->rsp_len != 2 + 12) {
		qbt_logf(bt, "QCA Version size mismatch len %zu", bt->rsp_len);
		return (-EILSEQ);
	}
	if (bt->rsp[0] != EDL_CMD_REQ_RES_EVT ||
	    bt->rsp[1] != EDL_APP_VER_RES_EVT) {
		qbt_logf(bt, "QCA Wrong packet received %d %d", bt->rsp[0],
		    bt->rsp[1]);
		return (-EIO);
	}
	ver->product_id = le32(bt->rsp + 2);
	ver->patch_ver = le16(bt->rsp + 6);
	ver->rom_ver = le16(bt->rsp + 8);
	ver->soc_id = le32(bt->rsp + 10);
	qbt_logf(bt, "QCA Product ID   :0x%08x", ver->product_id);
	qbt_logf(bt, "QCA SOC Version  :0x%08x", ver->soc_id);
	qbt_logf(bt, "QCA ROM Version  :0x%08x", ver->rom_ver);
	qbt_logf(bt, "QCA Patch Version:0x%08x", ver->patch_ver);
	if (ver->soc_id == 0 || ver->rom_ver == 0)
		return (-EILSEQ);
	return (0);
}

/* qca_uart_setup: the files go by the ROM version */
uint8_t
qbt_rom_version(const struct qbt_version *ver)
{
	uint32_t soc_ver = ver->soc_id << 16 | ver->rom_ver;

	return ((uint8_t)(((soc_ver & 0x00000f00) >> 4) |
	    (soc_ver & 0x0000000f)));
}

/*
 * qca_set_speed + qca_set_baudrate for the WCN399x: flow control off (RTS
 * deasserted) around the change, the command written raw, a moment for
 * the controller, then the host follows.
 */
int
qbt_set_baudrate(struct qbt *bt, unsigned int speed)
{
	uint8_t f[] = { H4_CMD, QCA_SET_BAUDRATE_OPCODE & 0xff,
	    QCA_SET_BAUDRATE_OPCODE >> 8, 1, qbt_baud_code(speed) };
	int error;

	if (f[4] > QCA_BAUDRATE_3200000)
		return (-EINVAL);
	bt->ops->set_flow(bt->arg, false);
	error = raw_write(bt, f, sizeof(f));
	bt->ops->drain(bt->arg);
	bt->ops->sleep_ms(bt->arg, 10);
	if (error == 0)
		error = bt->ops->set_speed(bt->arg, speed);
	bt->ops->set_flow(bt->arg, true);
	if (error == 0)
		qbt_logf(bt, "UART at %u baud", speed);
	return (error);
}

/* qca_tlv_check_data, for the WCN3950 (patch 0013 included) */
int
qbt_tlv_check(uint8_t *data, size_t size, int type, uint8_t baud, int *mode)
{
	uint32_t type_len, length, idx;
	uint16_t tag_id, tag_len;
	uint8_t *tlv, *nvm, t;

	*mode = QCA_SKIP_EVT_NONE;
	if (type == 1) {
		if (size < TLV_HDR + TLV_PATCH)
			return (-EINVAL);
		/* download_mode: after total, data length, format, signature */
		*mode = data[TLV_HDR + 10];
		return (0);
	}
	if (size < TLV_HDR)
		return (-EINVAL);
	tlv = data;
	type_len = le32(tlv);
	length = type_len >> 8;
	t = type_len & 0xff;
	/* more than one set: an enclosing type 4, the first set parsed */
	if (t == 4) {
		if (size < 2 * TLV_HDR)
			return (-EINVAL);
		tlv += TLV_HDR;
		type_len = le32(tlv);
		length = type_len >> 8;
		t = type_len & 0xff;
	}
	if (t != 2)
		return (0);
	if (size < length + (size_t)(tlv + TLV_HDR - data))
		return (-EINVAL);
	tlv += TLV_HDR;
	for (idx = 0; idx + TLV_NVM < length;
	    idx += TLV_NVM + tag_len) {
		nvm = tlv + idx;
		tag_id = le16(nvm);
		tag_len = le16(nvm + 2);
		if (length < idx + TLV_NVM + tag_len)
			return (-EINVAL);
		switch (tag_id) {
		case EDL_TAG_ID_HCI:
			if (tag_len < 3)
				return (-EINVAL);
			/* in-band sleep on the controller's side */
			nvm[TLV_NVM] |= 0x80;
			/* the rate: byte 1 for the WCN3950 too (patch 0013) */
			nvm[TLV_NVM + 1] = baud;
			break;
		case EDL_TAG_ID_DEEP_SLEEP:
			if (tag_len < 1)
				return (-EINVAL);
			nvm[TLV_NVM] |= 0x01;
			break;
		}
	}
	return (0);
}

/* qca_download_firmware + qca_tlv_send_segment */
int
qbt_download(struct qbt *bt, const char *name, int type)
{
	uint8_t *data, cmd[2 + MAX_SIZE_PER_TLV_SEGMENT];
	size_t size, off, seg;
	int error, mode, dnld_type, nseg = 0;

	qbt_logf(bt, "QCA Downloading %s", name);
	if ((error = bt->ops->fw_load(bt->arg, name, &data, &size)) != 0) {
		qbt_logf(bt, "QCA Failed to request file: %s (%d)", name,
		    error);
		return (error);
	}
	error = qbt_tlv_check(data, size, type, qbt_baud_code(bt->oper_speed),
	    &mode);
	if (error != 0)
		goto out;
	dnld_type = mode;
	for (off = 0; off < size; off += seg) {
		seg = size - off < MAX_SIZE_PER_TLV_SEGMENT ? size - off :
		    MAX_SIZE_PER_TLV_SEGMENT;
		/* the last segment is always acknowledged */
		if (off + seg == size || seg < MAX_SIZE_PER_TLV_SEGMENT)
			mode = QCA_SKIP_EVT_NONE;
		cmd[0] = EDL_PATCH_TLV_REQ_CMD;
		cmd[1] = (uint8_t)seg;
		memcpy(cmd + 2, data + off, seg);
		nseg++;
		if (mode == QCA_SKIP_EVT_VSE_CC || mode == QCA_SKIP_EVT_VSE) {
			error = qbt_cmd(bt, EDL_PATCH_CMD_OPCODE, cmd,
			    (uint8_t)(seg + 2), QBT_NO_WAIT, 0);
			if (error != 0)
				goto out;
			continue;
		}
		error = qbt_cmd(bt, EDL_PATCH_CMD_OPCODE, cmd,
		    (uint8_t)(seg + 2), HCI_EV_VENDOR, QBT_INIT_TIMEOUT_MS);
		if (error != 0) {
			qbt_logf(bt, "QCA Failed to send TLV segment (%d)",
			    error);
			goto out;
		}
		if (bt->rsp_len != 3) {
			qbt_logf(bt, "QCA TLV response size mismatch");
			error = -EILSEQ;
			goto out;
		}
		if (bt->rsp[0] != EDL_CMD_REQ_RES_EVT ||
		    bt->rsp[1] != EDL_TVL_DNLD_RES_EVT) {
			qbt_logf(bt, "QCA TLV with error stat 0x%x rtype 0x%x",
			    bt->rsp[0], bt->rsp[1]);
			error = -EIO;
			goto out;
		}
		if (bt->rsp[2] != 0)
			qbt_logf(bt, "QCA TLV with error stat 0x%x rtype 0x%x "
			    "(0x%x)", bt->rsp[0], bt->rsp[1], bt->rsp[2]);
	}
	/* no Command Complete for the skipped ones: the credit back */
	if (dnld_type == QCA_SKIP_EVT_VSE_CC || dnld_type == QCA_SKIP_EVT_VSE)
		bt->credits = 1;
	qbt_logf(bt, "%s: %zu bytes in %d segments (mode %d)", name, size,
	    nseg, dnld_type);
out:
	free(data);
	return (error);
}

static int
setup(struct qbt *bt)
{
	int error;

	if (bt->oper_speed != 115200 &&
	    (error = qbt_set_baudrate(bt, bt->oper_speed)) != 0)
		return (error);
	bt->rom = qbt_rom_version(&bt->ver);
	qbt_logf(bt, "QCA controller version 0x%08x",
	    bt->ver.soc_id << 16 | bt->ver.rom_ver);
	snprintf(bt->fw_rampatch, sizeof(bt->fw_rampatch),
	    "qca/cmbtfw%02x.tlv", bt->rom);
	snprintf(bt->fw_nvm, sizeof(bt->fw_nvm), "qca/cmnv%02x%s.bin",
	    bt->rom, bt->ver.soc_id == QCA_WCN3950_SOC_ID_T ? "t" :
	    bt->ver.soc_id == QCA_WCN3950_SOC_ID_S ? "s" : "");
	if ((error = qbt_download(bt, bt->fw_rampatch, 1)) != 0)
		return (error);
	/* time for the controller to get ready for the NVM */
	bt->ops->sleep_ms(bt->arg, 10);
	if ((error = qbt_download(bt, bt->fw_nvm, 2)) != 0)
		return (error);
	return (0);
}

static int
reset(struct qbt *bt)
{
	int error;

	error = qbt_cmd(bt, HCI_OP_RESET, NULL, 0, QBT_WAIT_CC,
	    QBT_INIT_TIMEOUT_MS);
	if (error == 0 && bt->status != 0)
		error = -EIO;
	if (error != 0)
		qbt_logf(bt, "QCA Reset failed (%d, status %d)", error,
		    bt->status);
	return (error);
}

/* qca_setup with qca_power_on/qca_regulator_init (the rails are on) */
int
qbt_power_on(struct qbt *bt, int tries)
{
	int error = -EIO, i;

	bt->up = false;
	for (i = 0; i < tries; i++) {
		if (i > 0)
			qbt_logf(bt, "Retry BT power ON:%d", i - 1);
		bt->ibs = false;
		bt->credits = 1;
		bt->rx_len = 0;
		/* into boot mode: the off pulse at 2400, then on at 115200 */
		if ((error = bt->ops->set_speed(bt->arg, 2400)) != 0 ||
		    (error = power_pulse(bt, false)) != 0 ||
		    (error = bt->ops->set_speed(bt->arg, 115200)) != 0 ||
		    (error = power_pulse(bt, true)) != 0)
			return (error);
		/* qca_port_reopen: in sync from here on */
		bt->ops->flush_input(bt->arg);
		bt->ops->set_flow(bt->arg, true);
		error = qbt_read_version(bt, &bt->ver);
		if (error != 0 && i == tries - 1 && bt->oper_speed != 115200) {
			/*
			 * The rails are the Wi-Fi's too: a controller that was
			 * not switched off may still run at the old speed.
			 */
			bt->ops->set_speed(bt->arg, bt->oper_speed);
			bt->ops->flush_input(bt->arg);
			if (qbt_read_version(bt, &bt->ver) == 0) {
				qbt_logf(bt, "the controller kept running at "
				    "%u baud", bt->oper_speed);
				if ((error = reset(bt)) == 0)
					goto up;
			}
		}
		if (error != 0)
			continue;
		if ((error = setup(bt)) != 0 || (error = reset(bt)) != 0)
			continue;
		goto up;
	}
	return (error);
up:
	/*
	 * In-band sleep from here on. The controller runs awake after the
	 * download until its SLEEP_IND (Leste's hci_qca: rx awake); we
	 * start asleep and wake it for the first packet.
	 */
	bt->ibs = true;
	bt->tx_state = IBS_TX_ASLEEP;
	bt->rx_awake = true;
	bt->up = true;
	qbt_logf(bt, "QCA setup on UART is completed");
	return (0);
}

int
qbt_power_off(struct qbt *bt)
{
	int error = 0;

	if (bt->up) {
		/* qca_send_pre_shutdown_cmd */
		error = qbt_cmd(bt, QCA_PRE_SHUTDOWN_CMD, NULL, 0, QBT_WAIT_CC,
		    QBT_CMD_TIMEOUT_MS);
		bt->ops->sleep_ms(bt->arg, 10);
	}
	/*
	 * The WCN3950 gets no power-off pulse in qca_power_off (only the
	 * WCN3988/3990/3991/3998 do); its rails are the caller's.
	 */
	bt->ibs = false;
	bt->up = false;
	bt->tx_state = IBS_TX_ASLEEP;
	return (error);
}
