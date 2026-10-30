/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The remote processors' device (/dev/qcom_pas/<name>, qcom_pas.c) as the
 * loader (pro1x-dsp) uses it: the firmware's metadata to TrustZone, its
 * segments into the processor's memory, then the start.
 */

#ifndef _QCOM_PAS_IOCTL_H_
#define	_QCOM_PAS_IOCTL_H_

#include <sys/types.h>
#ifdef __FreeBSD__
#include <sys/ioccom.h>
#else
#include <sys/ioctl.h>	/* the loader's dry run on other systems */
#endif

/* The ELF header with the program headers, then the hash segment */
struct qcom_pas_init {
	uint64_t	meta;		/* user address */
	uint64_t	meta_len;
};

/* filesz bytes from data, then zeros up to memsz, at offset */
struct qcom_pas_segment {
	uint64_t	offset;		/* in the processor's memory region */
	uint64_t	data;		/* user address */
	uint64_t	filesz;
	uint64_t	memsz;
};

#define	QCOM_PAS_STATE_OFF	0
#define	QCOM_PAS_STATE_LOADING	1	/* metadata handed over */
#define	QCOM_PAS_STATE_RUNNING	2	/* reported ready */
#define	QCOM_PAS_STATE_CRASHED	3	/* fatal error or watchdog */

struct qcom_pas_status {
	uint32_t	pas_id;
	uint32_t	state;
	uint64_t	mem_phys;
	uint64_t	mem_size;
	uint32_t	smp2p_in;	/* the remote's state bits */
	uint32_t	flags;		/* QCOM_PAS_F_* seen since the start */
	char		crash_reason[128];
	uint32_t	ms;		/* since the last start (or adoption) */
	uint32_t	spare[3];
};
#define	QCOM_PAS_F_FATAL	0x01
#define	QCOM_PAS_F_READY	0x02
#define	QCOM_PAS_F_HANDOVER	0x04
#define	QCOM_PAS_F_STOP_ACK	0x08
#define	QCOM_PAS_F_WDOG		0x10
#define	QCOM_PAS_F_ADOPTED	0x20	/* started by Linux, taken over */

#define	QCOM_PAS_INIT		_IOW('Q', 1, struct qcom_pas_init)
#define	QCOM_PAS_MEM_SETUP	_IOW('Q', 2, uint64_t)	/* relocated size */
#define	QCOM_PAS_LOAD		_IOW('Q', 3, struct qcom_pas_segment)
#define	QCOM_PAS_START		_IOW('Q', 4, uint32_t)	/* timeout, ms */
#define	QCOM_PAS_STOP		_IO('Q', 5)
#define	QCOM_PAS_STATUS		_IOR('Q', 6, struct qcom_pas_status)
#define	QCOM_PAS_SHUTDOWN	_IO('Q', 7)		/* TrustZone only */
/* a processor Linux started and left running: taken over (no TrustZone) */
#define	QCOM_PAS_ADOPT		_IO('Q', 8)

#endif /* _QCOM_PAS_IOCTL_H_ */
