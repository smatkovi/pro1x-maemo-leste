/*
 * What Maemo's code takes from glibc and FreeBSD's libc spells otherwise
 * or lacks; included before every source file of the Hildon parts
 * (freebsd/hildon/build.sh).
 */
#ifndef PRO1X_GLIBC_COMPAT_H
#define PRO1X_GLIBC_COMPAT_H

/* Retry a system call interrupted by a signal (glibc's <unistd.h>) */
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expr) \
	(__extension__({ long __r; \
	    do __r = (long)(expr); while (__r == -1L && errno == EINTR); \
	    __r; }))
#endif

/* Leap year (glibc's <time.h> internals, which some Maemo code uses) */
#ifndef __isleap
#define __isleap(year) \
	((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))
#endif

/* The longest host name (POSIX leaves it to <limits.h>; FreeBSD has none) */
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#endif
