/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The FastRPC device (/dev/fastrpc-<dsp>, qcom_fastrpc.c) as Linux's
 * <misc/fastrpc.h> has it: the same structures, FreeBSD's ioctl encoding.
 * Ported programs (hexagonrpcd) include this as misc/fastrpc.h.
 */

#ifndef _FASTRPC_IOCTL_H_
#define	_FASTRPC_IOCTL_H_

#include <sys/types.h>
#include <sys/ioccom.h>

struct fastrpc_invoke_args {
	uint64_t	ptr;
	uint64_t	length;
	int32_t		fd;
	uint32_t	attr;
};

struct fastrpc_invoke {
	uint32_t	handle;
	uint32_t	sc;
	uint64_t	args;		/* struct fastrpc_invoke_args[] */
};

struct fastrpc_init_create {
	uint32_t	filelen;
	int32_t		filefd;
	uint32_t	attrs;
	uint32_t	siglen;
	uint64_t	file;
};

struct fastrpc_alloc_dma_buf {
	int32_t		fd;
	uint32_t	flags;
	uint64_t	size;
};

struct fastrpc_req_mmap {
	int32_t		fd;
	uint32_t	flags;
	uint64_t	vaddrin;
	uint64_t	size;
	uint64_t	vaddrout;
};

struct fastrpc_req_munmap {
	uint64_t	vaddrout;
	uint64_t	size;
};

#define	FASTRPC_IOCTL_ALLOC_DMA_BUFF	_IOWR('R', 1, struct fastrpc_alloc_dma_buf)
#define	FASTRPC_IOCTL_INVOKE		_IOWR('R', 3, struct fastrpc_invoke)
#define	FASTRPC_IOCTL_INIT_ATTACH	_IO('R', 4)
#define	FASTRPC_IOCTL_INIT_CREATE	_IOWR('R', 5, struct fastrpc_init_create)
#define	FASTRPC_IOCTL_MMAP		_IOWR('R', 6, struct fastrpc_req_mmap)
#define	FASTRPC_IOCTL_MUNMAP		_IOWR('R', 7, struct fastrpc_req_munmap)
#define	FASTRPC_IOCTL_INIT_ATTACH_SNS	_IO('R', 8)

#endif /* _FASTRPC_IOCTL_H_ */
