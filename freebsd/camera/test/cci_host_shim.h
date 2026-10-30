/*
 * Host test shim for sys/arm64/qualcomm/camss/qcom_cci.c (CCI_HOST_TEST):
 * the register accesses go to the CCI model in test_cci.c.
 */
#ifndef _CCI_HOST_SHIM_H_
#define	_CCI_HOST_SHIM_H_

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef struct host_dev *device_t;
struct resource { int dummy; };
struct mtx { int dummy; };
#define	mtx_lock(m)		((void)0)
#define	mtx_unlock(m)		((void)0)
#define	device_printf(d, ...)	printf("cci: " __VA_ARGS__)
typedef uint32_t phandle_t;
#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

uint32_t model_read(uint32_t reg);
void	model_write(uint32_t reg, uint32_t val);
void	model_delay(int us);
#define	bus_read_4(r, o)	model_read(o)
#define	bus_write_4(r, o, v)	model_write((o), (v))
#define	DELAY(us)		model_delay(us)

/* sys/dev/iicbus/iic.h, iiconf.h */
struct iic_msg {
	uint16_t	slave;
	uint16_t	flags;
#define	IIC_M_WR	0
#define	IIC_M_RD	0x0001
#define	IIC_M_NOSTOP	0x0002
	uint16_t	len;
	uint8_t		*buf;
};
#define	IIC_EBUSERR	0x1
#define	IIC_ENOACK	0x2
#define	IIC_ETIMEOUT	0x3
#define	IIC_EOVERFLOW	0x7
#define	IIC_ENOTSUPP	0x8
#include <errno.h>

#endif
