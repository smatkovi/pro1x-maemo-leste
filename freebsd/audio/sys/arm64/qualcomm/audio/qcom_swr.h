/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The SoundWire masters' interrupts for pro1x-audiod (qcom_swr.ko,
 * freebsd/audio/README.md, "The headset jack"). The masters themselves are
 * the daemon's (swr.c, through /dev/qcom_lpass); the kernel only takes
 * their interrupt lines, as Linux's uio_pdrv_genirq does for a driver in
 * user space:
 *
 *   /dev/qcom_swr0     swr0 (0x0a740000, the WCD9370's TX device and its
 *                      registers): QCOM_SWR_MASTER, the master's interrupt
 *                      (GIC SPI 296, MPM pin 5), and QCOM_SWR_WAKE, its
 *                      wake-up interrupt while the bus clock is stopped
 *                      (SPI 79, MPM pin 24)
 *   /dev/qcom_swr1     swr1 (0x0a610000, the RX device): QCOM_SWR_MASTER
 *                      (SPI 297)
 *
 * Each interrupt is masked in the GIC until it is armed (QCOM_SWR_ARM),
 * and masked again as soon as it fires: the kernel never touches the
 * master's registers (its clock may be off: a read of the unclocked block
 * hangs the bus), so the line stays asserted until the daemon has read
 * and cleared the master's status and arms the interrupt again. read(2)
 * gives a struct qcom_swr_event once something fired (blocking unless
 * O_NONBLOCK; poll(2) says POLLIN); closing the device masks both.
 */

#ifndef _QCOM_SWR_H_
#define	_QCOM_SWR_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	QCOM_SWR_DEV		"/dev/qcom_swr"	/* + the master's number */
#define	QCOM_SWR_MASTER		0x1
#define	QCOM_SWR_WAKE		0x2

struct qcom_swr_event {
	uint32_t	fired;		/* QCOM_SWR_*: fired since the last read */
	uint32_t	armed;		/* still armed */
	uint64_t	count[2];	/* master, wake: fired since attach */
};

struct qcom_swr_info {
	uint32_t	have;		/* QCOM_SWR_*: the interrupts it has */
	uint32_t	intid[2];	/* their GIC INTIDs (SPI + 32), 0: none */
	uint64_t	pa;		/* the master's registers */
};

#define	QCOM_SWR_ARM		_IOW('W', 1, uint32_t)	/* unmask, once */
#define	QCOM_SWR_DISARM		_IOW('W', 2, uint32_t)
#define	QCOM_SWR_INFO		_IOR('W', 3, struct qcom_swr_info)

#endif /* _QCOM_SWR_H_ */
