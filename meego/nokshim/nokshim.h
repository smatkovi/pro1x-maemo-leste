/*
 * Protocol between libEGL-nokshim (in every Harmattan GL process) and
 * nok-image-broker, which keeps the dma-buf file descriptors of shared
 * images so that other processes can import them by handle.
 */
#ifndef NOKSHIM_H
#define NOKSHIM_H

#include <stdint.h>

#define NOK_BROKER_PATH "/usr/libexec/nok-image-broker"
#define NOK_MAX_PLANES 4

enum nok_op {
	NOK_REGISTER = 1,	/* + fds; reply: handle */
	NOK_LOOKUP = 2,		/* reply: image + fds */
	NOK_DESTROY = 3,	/* no reply */
};

struct nok_image {
	uint32_t width, height;
	uint32_t fourcc;
	uint32_t planes;
	uint64_t modifier;
	uint32_t strides[NOK_MAX_PLANES];
	uint32_t offsets[NOK_MAX_PLANES];
};

struct nok_msg {
	uint32_t op;
	uint32_t handle;	/* 0: failure in replies */
	struct nok_image image;
};

/* Abstract socket name, one broker per user. */
static inline int nok_socket_name(char *buf, int len, unsigned uid)
{
	int n = 0;
	const char *p = "pro1x-nok-image-";
	buf[n++] = '\0';
	while (*p && n < len)
		buf[n++] = *p++;
	char digits[12];
	int d = 0;
	do {
		digits[d++] = '0' + uid % 10;
		uid /= 10;
	} while (uid && d < 11);
	while (d && n < len)
		buf[n++] = digits[--d];
	return n;
}

#endif
