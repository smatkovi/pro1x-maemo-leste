/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Register access over /dev/iicN (iic(4), I2CRDWR): 16-bit register
 * addresses big-endian (the PM8008's regmap, the sensors' CCI registers),
 * 8-bit ones (the PCA9534, the DW9800). A read is a write of the address
 * and a read in one transfer (a repeated start on the PMIC's bus; the CCI
 * driver puts a stop between them, as Linux's CCI driver does). FreeBSD's
 * I2C slave addresses are 8 bit: the 7-bit address shifted left by one.
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <dev/iicbus/iic.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cam.h"

static int iic_fd[32];
static int iic_init;

int
iic_open(int unit)
{
	char path[32];
	u_int i;

	if (!iic_init) {
		for (i = 0; i < nitems(iic_fd); i++)
			iic_fd[i] = -1;
		iic_init = 1;
	}
	if (unit < 0 || unit >= (int)nitems(iic_fd)) {
		errno = ENXIO;
		return (-1);
	}
	if (iic_fd[unit] < 0) {
		snprintf(path, sizeof(path), "/dev/iic%d", unit);
		iic_fd[unit] = open(path, O_RDWR);
	}
	return (iic_fd[unit]);
}

static int
xfer(int unit, struct iic_msg *m, int n)
{
	struct iic_rdwr_data d;
	int fd;

	if ((fd = iic_open(unit)) < 0)
		return (-1);
	d.msgs = m;
	d.nmsgs = n;
	return (ioctl(fd, I2CRDWR, &d));
}

static int
rd(int unit, uint8_t addr, const uint8_t *a, u_int alen, uint8_t *buf,
    u_int len)
{
	struct iic_msg m[2];

	m[0].slave = addr << 1;
	m[0].flags = IIC_M_WR | IIC_M_NOSTOP;
	m[0].len = alen;
	m[0].buf = (uint8_t *)(uintptr_t)a;
	m[1].slave = addr << 1;
	m[1].flags = IIC_M_RD;
	m[1].len = len;
	m[1].buf = buf;
	return (xfer(unit, m, 2));
}

static int
wr(int unit, uint8_t addr, const uint8_t *a, u_int alen, const uint8_t *buf,
    u_int len)
{
	struct iic_msg m;
	uint8_t b[64];

	if (alen + len > sizeof(b)) {
		errno = EINVAL;
		return (-1);
	}
	memcpy(b, a, alen);
	memcpy(b + alen, buf, len);
	m.slave = addr << 1;
	m.flags = IIC_M_WR;
	m.len = alen + len;
	m.buf = b;
	return (xfer(unit, &m, 1));
}

int
iic_rd16(int unit, uint8_t addr, uint16_t reg, uint8_t *buf, u_int len)
{
	uint8_t a[2] = { reg >> 8, reg & 0xff };

	return (rd(unit, addr, a, 2, buf, len));
}

int
iic_wr16(int unit, uint8_t addr, uint16_t reg, const uint8_t *buf, u_int len)
{
	uint8_t a[2] = { reg >> 8, reg & 0xff };

	return (wr(unit, addr, a, 2, buf, len));
}

int
iic_rd8(int unit, uint8_t addr, uint8_t reg, uint8_t *buf, u_int len)
{
	return (rd(unit, addr, &reg, 1, buf, len));
}

int
iic_wr8(int unit, uint8_t addr, uint8_t reg, const uint8_t *buf, u_int len)
{
	return (wr(unit, addr, &reg, 1, buf, len));
}
