/* SPDX-License-Identifier: GPL-2.0-only */
/* The PC's stand-in for Linux's types, for Linux's IPA tables and if_rmnet.h */
#ifndef _SHIM_LINUX_TYPES_H_
#define	_SHIM_LINUX_TYPES_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __sum16;
#ifndef __aligned
#define	__aligned(x)	__attribute__((__aligned__(x)))
#endif
#endif
