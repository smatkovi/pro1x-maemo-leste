/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * /dev/camss: the Pro1X's camera subsystem for user space
 * (freebsd/camera/README.md). The kernel does what needs the SoC's
 * registers: the camera clocks and power domain, the pins, the CCI (its
 * two I2C buses are iicbus(4) buses, /dev/iicN), the CSIPHY, CSID and TFE
 * that write raw frames into memory. User space (pro1x-cam) does the
 * sequencing that is the board's and the sensors' business: the PMIC's
 * and the expander's rails over /dev/iicN, the sensors' registers over
 * the CCI, exposure, focus, the pictures.
 */
#ifndef _QCOM_CAMSS_IOCTL_H_
#define	_QCOM_CAMSS_IOCTL_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	CAMSS_ABI_VERSION	1

#define	CAMSS_MAX_SENSORS	4
#define	CAMSS_MAX_SUPPLIES	8
#define	CAMSS_MAX_LINKFREQ	4
#define	CAMSS_MAX_BUFS		8

/* where a rail comes from (the device tree's "-supply" phandles) */
#define	CAMSS_SUPPLY_OTHER	0	/* nothing user space can switch */
#define	CAMSS_SUPPLY_PCA9534	1	/* regulator-fixed on an expander pin */
#define	CAMSS_SUPPLY_PM8008	2	/* an LDO of the PM8008 */

struct camss_supply {
	char		name[16];	/* "vana" (property vana-supply) */
	char		node[32];	/* the regulator's node name */
	uint8_t		type;		/* CAMSS_SUPPLY_* */
	uint8_t		index;		/* expander pin, or LDO number */
	uint8_t		active_high;	/* expander pin: enable-active-high */
	uint8_t		addr;		/* the chip's 7-bit I2C address */
	int16_t		iic_unit;	/* its bus: /dev/iicN, -1 not found */
	uint16_t	pad;
	uint32_t	min_uv, max_uv;	/* regulator-min/max-microvolt */
};

struct camss_sensor {
	char		node[32];	/* "camera@10" */
	char		compatible[32];	/* "sony,imx582" */
	uint8_t		port;		/* CAMSS port = CSIPHY */
	uint8_t		nlanes;		/* data-lanes of the CAMSS endpoint */
	uint8_t		lanes[4];	/* their positions */
	uint8_t		cci_master;	/* i2c-bus@N of the CCI */
	uint8_t		addr;		/* 7-bit I2C address */
	int16_t		iic_unit;	/* the CCI bus as /dev/iicN, -1 none */
	int16_t		reset_gpio;	/* TLMM pin, -1 none */
	uint8_t		reset_active_low;
	uint8_t		nsupplies;
	uint8_t		nlink;
	uint8_t		has_lens;
	int16_t		mclk;		/* MCLK index (GCC_CAMSS_MCLKn), -1 */
	uint32_t	mclk_hz;	/* assigned-clock-rates */
	int32_t		orientation;	/* -1 if absent */
	int32_t		rotation;	/* -1 if absent */
	uint64_t	link_freq[CAMSS_MAX_LINKFREQ];
	struct camss_supply supply[CAMSS_MAX_SUPPLIES];
	/* the lens (lens-focus): a voice coil on the same CCI bus */
	char		lens_node[32];
	char		lens_compatible[32];
	uint8_t		lens_addr;
	uint8_t		nlens_supplies;
	uint16_t	pad;
	struct camss_supply lens_supply[CAMSS_MAX_SUPPLIES];
};

struct camss_info {
	uint32_t	version;	/* CAMSS_ABI_VERSION */
	uint32_t	nsensors;
	/* the PMIC behind the camera rails (pm8008): its reset line */
	int16_t		pmic_reset_gpio;	/* TLMM pin, -1 none */
	uint8_t		pmic_reset_active_low;
	uint8_t		pad;
	uint32_t	cci_hw_version;
	struct camss_sensor sensor[CAMSS_MAX_SENSORS];
};

/* the sensor's MCLK (and its pins: the sensor node's pinctrl-0) */
struct camss_mclk {
	uint32_t	sensor;		/* index in camss_info */
	uint32_t	on;
	uint32_t	hz;		/* out: the rate it runs at */
};

/*
 * One of the lines camss_info names (a sensor's reset, the PMIC's reset),
 * driven as a GPIO: value 1 = asserted (active), 0 = deasserted; the
 * polarity is the device tree's.
 */
struct camss_gpio {
	int32_t		gpio;
	uint32_t	value;
};

/* --- stage 2: raw frames (CSIPHY -> CSID RDI0 -> TFE write master) ------ */

struct camss_stream {
	uint32_t	sensor;		/* index in camss_info */
	uint32_t	width, height;	/* pixels */
	uint32_t	bpp;		/* 10 (RAW10, CSI-2 data type 0x2b) */
	uint32_t	nbufs;		/* 2 .. CAMSS_MAX_BUFS */
	uint64_t	link_freq;	/* Hz, the sensor's mode */
	uint64_t	pixel_rate;	/* Hz, the sensor's mode */
	/* out */
	uint32_t	stride;		/* bytes per line (packed, 8-byte aligned) */
	uint32_t	bufsize;	/* bytes per buffer, page aligned: mmap */
	uint32_t	clk_tfe, clk_csid, clk_cphy, clk_timer;	/* Hz */
	uint32_t	settle_cnt;
};

struct camss_buf {
	uint32_t	index;		/* in: QBUF; out: DQBUF */
	uint32_t	timeout_ms;	/* DQBUF: wait at most this long */
	uint32_t	sequence;	/* out: frame number */
	uint32_t	flags;		/* out: CAMSS_BUF_* */
	uint64_t	timestamp_ns;	/* out: when the TFE reported it */
	uint32_t	tfe_status, bus_status;	/* out: the interrupt status */
	uint32_t	overflow, violation;	/* out: bus errors seen */
};
#define	CAMSS_BUF_ERROR		0x1	/* overflow or violation on the way */

/* a register window for diagnostics: "csiphy0", "csid0", "vfe0", "top" */
struct camss_regdump {
	char		name[16];
	uint32_t	offset;		/* bytes into the window */
	uint32_t	count;		/* registers, at most 64 */
	uint32_t	val[64];
};

#define	CAMSS_IOC_INFO		_IOR('C', 1, struct camss_info)
#define	CAMSS_IOC_MCLK		_IOWR('C', 2, struct camss_mclk)
#define	CAMSS_IOC_GPIO		_IOW('C', 3, struct camss_gpio)
#define	CAMSS_IOC_STREAMON	_IOWR('C', 10, struct camss_stream)
#define	CAMSS_IOC_STREAMOFF	_IO('C', 11)
#define	CAMSS_IOC_QBUF		_IOW('C', 12, struct camss_buf)
#define	CAMSS_IOC_DQBUF		_IOWR('C', 13, struct camss_buf)
#define	CAMSS_IOC_REGDUMP	_IOWR('C', 14, struct camss_regdump)

#endif /* _QCOM_CAMSS_IOCTL_H_ */
