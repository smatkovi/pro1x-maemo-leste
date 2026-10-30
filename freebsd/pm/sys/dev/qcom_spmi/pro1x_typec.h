/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The PMI632's Type-C block and VBUS output (pro1x_typec.c), after Linux
 * 7.1's qcom_pmic_typec_port.c and qcom_usb_vbus-regulator.c; plain C, so
 * that the host test (freebsd/pm/test/typec_test.c) checks the decoding.
 */
#ifndef _PRO1X_TYPEC_H_
#define	_PRO1X_TYPEC_H_

#define	TYPEC_SNK_STATUS	0x06
#define	TYPEC_SNK_RP_STD	(1u << 3)
#define	TYPEC_SNK_RP_1P5	(1u << 2)
#define	TYPEC_SNK_RP_3P0	(1u << 1)
#define	TYPEC_SRC_STATUS	0x08
#define	TYPEC_SRC_DEBUG_ACCESS	(1u << 4)
#define	TYPEC_SRC_RD_OPEN	(1u << 3)
#define	TYPEC_SRC_RD_RA_VCONN	(1u << 2)
#define	TYPEC_SRC_RA_OPEN	(1u << 1)
#define	TYPEC_SRC_AUDIO_RA_RA	(1u << 0)
#define	TYPEC_MISC_STATUS	0x0b
#define	TYPEC_SNK_SRC_MODE	(1u << 6)
#define	TYPEC_VBUS_DETECT	(1u << 5)
#define	TYPEC_CC_ORIENTATION	(1u << 1)
#define	TYPEC_CC_ATTACHED	(1u << 0)
#define	TYPEC_MODE_CFG		0x44
#define	TYPEC_POWER_ROLE_MASK	7u
#define	TYPEC_EN_SRC_ONLY	(1u << 2)
#define	TYPEC_EN_SNK_ONLY	(1u << 1)
#define	TYPEC_DISABLE_CMD	(1u << 0)
/* the VBUS output, from its own base */
#define	TYPEC_CMD_OTG		0x40
#define	TYPEC_OTG_EN		(1u << 0)
#define	TYPEC_OTG_CURRENT_LIMIT_CFG 0x52
#define	TYPEC_OTG_CFG		0x53
#define	TYPEC_OTG_EN_SRC_CFG	(1u << 1)

enum typec_role {
	TYPEC_ROLE_NONE,
	TYPEC_ROLE_DEVICE,	/* we are the sink: a host (or a charger) */
	TYPEC_ROLE_HOST,	/* we are the source, a device's Rd */
	TYPEC_ROLE_AUDIO,	/* Ra/Ra: an audio accessory */
	TYPEC_ROLE_DEBUG,
};

struct typec_state {
	int	attached;	/* -1 before the first look */
	int	role;
	int	cc2;		/* the orientation: CC2 (flipped) */
	int	vbus;
	int	rp_ma;		/* as a sink: 500 default, 1500, 3000 */
};

static inline void
typec_decode(uint8_t misc, uint8_t snk, uint8_t src, struct typec_state *st)
{
	st->attached = (misc & TYPEC_CC_ATTACHED) != 0;
	st->cc2 = st->attached && (misc & TYPEC_CC_ORIENTATION) != 0;
	st->vbus = (misc & TYPEC_VBUS_DETECT) != 0;
	st->role = TYPEC_ROLE_NONE;
	st->rp_ma = 0;
	if (!st->attached)
		return;
	if (misc & TYPEC_SNK_SRC_MODE) {
		if (src & TYPEC_SRC_AUDIO_RA_RA)
			st->role = TYPEC_ROLE_AUDIO;
		else if (src & TYPEC_SRC_DEBUG_ACCESS)
			st->role = TYPEC_ROLE_DEBUG;
		else if (src & (TYPEC_SRC_RD_OPEN | TYPEC_SRC_RD_RA_VCONN))
			st->role = TYPEC_ROLE_HOST;
	} else {
		st->role = TYPEC_ROLE_DEVICE;
		st->rp_ma = (snk & TYPEC_SNK_RP_3P0) ? 3000 :
		    (snk & TYPEC_SNK_RP_1P5) ? 1500 :
		    (snk & TYPEC_SNK_RP_STD) ? 500 : 0;
	}
}

static inline const char *
typec_role_name(int role)
{
	static const char *const n[] = { "none", "device", "host", "audio",
	    "debug" };

	return (role >= 0 && role <= TYPEC_ROLE_DEBUG ? n[role] : "?");
}

static inline const char *
typec_mode_name(uint8_t mode)
{
	if (mode & TYPEC_DISABLE_CMD)
		return ("disabled");
	if (mode & TYPEC_EN_SRC_ONLY)
		return ("source only");
	if (mode & TYPEC_EN_SNK_ONLY)
		return ("sink only");
	return ("dual role");
}

/* OTG_CURRENT_LIMIT_CFG: 500 mA steps from 500 mA, at most 3 A */
static inline uint8_t
typec_vbus_limit(uint32_t ua)
{
	uint32_t i = ua < 500000 ? 0 : (ua - 500000) / 500000;

	return (i > 5 ? 5 : (uint8_t)i);
}

static inline void
typec_format(const struct typec_state *st, char *buf, size_t len)
{
	snprintf(buf, len, "role=%s orientation=%s vbus=%d rp=%d",
	    st->attached ? typec_role_name(st->role) : "none",
	    st->attached ? (st->cc2 ? "cc2" : "cc1") : "none", st->vbus,
	    st->rp_ma);
}
#endif
