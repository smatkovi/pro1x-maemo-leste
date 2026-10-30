/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * What the IPA's core (gsi.c, ipa_hw.c, qmap.c) needs of its system: locks,
 * a completion, sleeping, logging. In the kernel these are FreeBSD's; on the
 * PC (freebsd/data/test) pthreads, so that the same core runs against the
 * tests' model of the hardware. The registers, DMA memory and the deliveries
 * to the network stack are functions of the system layer (ipa_var.h:
 * ipa_os_*), ipa_fbsd.c in the kernel.
 */

#ifndef _IPA_OS_H_
#define	_IPA_OS_H_

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/condvar.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>

MALLOC_DECLARE(M_QCOM_IPA);

typedef struct mtx	ipa_mutex_t;
#define	ipa_mutex_init(m, n)	mtx_init((m), (n), NULL, MTX_DEF)
#define	ipa_mutex_destroy(m)	mtx_destroy(m)
#define	ipa_lock(m)		mtx_lock(m)
#define	ipa_unlock(m)		mtx_unlock(m)
/* held across sleeps (the GSI's commands) */
typedef struct sx	ipa_sleeplock_t;
#define	ipa_sleeplock_init(l, n) sx_init((l), (n))
#define	ipa_sleeplock_destroy(l) sx_destroy(l)
#define	ipa_slock(l)		sx_xlock(l)
#define	ipa_sunlock(l)		sx_xunlock(l)

struct ipa_completion {
	struct mtx	mtx;
	struct cv	cv;
	u_int		done;
};
static inline void
ipa_completion_init(struct ipa_completion *c)
{
	mtx_init(&c->mtx, "ipacompl", NULL, MTX_DEF);
	cv_init(&c->cv, "ipacompl");
	c->done = 0;
}
static inline void
ipa_completion_destroy(struct ipa_completion *c)
{
	cv_destroy(&c->cv);
	mtx_destroy(&c->mtx);
}
static inline void
ipa_completion_reinit(struct ipa_completion *c)
{
	mtx_lock(&c->mtx);
	c->done = 0;
	mtx_unlock(&c->mtx);
}
static inline void
ipa_complete(struct ipa_completion *c)
{
	mtx_lock(&c->mtx);
	c->done++;
	cv_broadcast(&c->cv);
	mtx_unlock(&c->mtx);
}
/* true when completed within ms */
static inline bool
ipa_completion_wait(struct ipa_completion *c, u_int ms)
{
	sbintime_t end = getsbinuptime() + (sbintime_t)ms * SBT_1MS, now;
	bool ok;

	mtx_lock(&c->mtx);
	while (c->done == 0 && (now = getsbinuptime()) < end)
		cv_timedwait_sbt(&c->cv, &c->mtx, end - now, 0, 0);
	ok = c->done != 0;
	if (ok)
		c->done--;
	mtx_unlock(&c->mtx);
	return (ok);
}
#define	ipa_msleep(ms)		pause_sbt("qcomipa", (sbintime_t)(ms) * SBT_1MS, \
				    SBT_1MS, 0)
#define	ipa_udelay(us)		DELAY(us)
#define	ipa_wmb()		wmb()
#define	ipa_rmb()		rmb()
#define	ipa_le64(x)		htole64(x)
#define	ipa_le32(x)		htole32(x)
#define	ipa_le16(x)		htole16(x)
#define	ipa_from_le64(x)	le64toh(x)
#define	ipa_from_le32(x)	le32toh(x)
#define	ipa_from_le16(x)	le16toh(x)
#define	ipa_zalloc(n)		malloc((n), M_QCOM_IPA, M_WAITOK | M_ZERO)
#define	ipa_free(p)		free((p), M_QCOM_IPA)

#else /* !_KERNEL: the tests on the PC */
#include <sys/types.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __packed
#define	__packed		__attribute__((__packed__))
#endif
typedef unsigned int		u_int;

typedef pthread_mutex_t		ipa_mutex_t;
#define	ipa_mutex_init(m, n)	pthread_mutex_init((m), NULL)
#define	ipa_mutex_destroy(m)	pthread_mutex_destroy(m)
#define	ipa_lock(m)		pthread_mutex_lock(m)
#define	ipa_unlock(m)		pthread_mutex_unlock(m)
typedef pthread_mutex_t		ipa_sleeplock_t;
#define	ipa_sleeplock_init(l, n) pthread_mutex_init((l), NULL)
#define	ipa_sleeplock_destroy(l) pthread_mutex_destroy(l)
#define	ipa_slock(l)		pthread_mutex_lock(l)
#define	ipa_sunlock(l)		pthread_mutex_unlock(l)

struct ipa_completion {
	pthread_mutex_t	mtx;
	pthread_cond_t	cv;
	u_int		done;
};
static inline void
ipa_completion_init(struct ipa_completion *c)
{
	pthread_mutex_init(&c->mtx, NULL);
	pthread_cond_init(&c->cv, NULL);
	c->done = 0;
}
static inline void
ipa_completion_destroy(struct ipa_completion *c)
{
	pthread_cond_destroy(&c->cv);
	pthread_mutex_destroy(&c->mtx);
}
static inline void
ipa_completion_reinit(struct ipa_completion *c)
{
	pthread_mutex_lock(&c->mtx);
	c->done = 0;
	pthread_mutex_unlock(&c->mtx);
}
static inline void
ipa_complete(struct ipa_completion *c)
{
	pthread_mutex_lock(&c->mtx);
	c->done++;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mtx);
}
static inline bool
ipa_completion_wait(struct ipa_completion *c, u_int ms)
{
	struct timespec ts;
	bool ok;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&c->mtx);
	while (c->done == 0)
		if (pthread_cond_timedwait(&c->cv, &c->mtx, &ts) != 0)
			break;
	ok = c->done != 0;
	if (ok)
		c->done--;
	pthread_mutex_unlock(&c->mtx);
	return (ok);
}
#define	ipa_msleep(ms)		usleep((ms) * 1000)
#define	ipa_udelay(us)		usleep(us)
#define	ipa_wmb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	ipa_rmb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	ipa_le64(x)		htole64(x)
#define	ipa_le32(x)		htole32(x)
#define	ipa_le16(x)		htole16(x)
#define	ipa_from_le64(x)	le64toh(x)
#define	ipa_from_le32(x)	le32toh(x)
#define	ipa_from_le16(x)	le16toh(x)
#define	ipa_zalloc(n)		calloc(1, (n))
#define	ipa_free(p)		free(p)
#ifndef MIN
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)		((a) > (b) ? (a) : (b))
#endif
#ifndef nitems
#define	nitems(x)		(sizeof(x) / sizeof((x)[0]))
#endif
#ifndef roundup2
#define	roundup2(x, y)		(((x) + ((y) - 1)) & (~((y) - 1)))
#endif
#endif /* !_KERNEL */

#endif /* _IPA_OS_H_ */
