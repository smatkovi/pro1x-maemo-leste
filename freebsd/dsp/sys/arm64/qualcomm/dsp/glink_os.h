/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * What glink_core.c needs of its surroundings: a mutex, a condition with a
 * timeout, memory, a clock, a log and a write barrier. In the kernel
 * (_KERNEL) FreeBSD's mtx(9), condvar(9) and malloc(9); elsewhere POSIX
 * threads, for the test on the PC.
 */

#ifndef _GLINK_OS_H_
#define	_GLINK_OS_H_

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/time.h>
#include <machine/atomic.h>

MALLOC_DECLARE(M_GLINK);

typedef struct mtx	glink_mutex_t;
typedef struct cv	glink_cond_t;

#define	glink_mutex_init(m)	mtx_init((m), "glink", NULL, MTX_DEF)
#define	glink_mutex_destroy(m)	mtx_destroy(m)
#define	glink_lock(m)		mtx_lock(m)
#define	glink_unlock(m)		mtx_unlock(m)
#define	glink_cond_init(c)	cv_init((c), "glink")
#define	glink_cond_destroy(c)	cv_destroy(c)
#define	glink_broadcast(c)	cv_broadcast(c)
/* 0, or EWOULDBLOCK after ms */
#define	glink_cond_wait(c, m, ms) \
	cv_timedwait_sbt((c), (m), (sbintime_t)(ms) * SBT_1MS, 0, 0)
#define	glink_zalloc(n)		malloc((n), M_GLINK, M_WAITOK | M_ZERO)
#define	glink_free(p)		free((p), M_GLINK)
#define	glink_now_ms()		((uint64_t)(getsbinuptime() / SBT_1MS))
#define	glink_log(...)		printf("glink: " __VA_ARGS__)
#define	glink_wmb()		wmb()
#define	GLINK_TIMEDOUT		EWOULDBLOCK

/* Deferred work: a task on the system's thread task queue */
#include <sys/taskqueue.h>
typedef struct task	glink_work_t;
#define	glink_work_init(w, fn, arg) \
	TASK_INIT((w), 0, (void (*)(void *, int))(fn), (arg))
#define	glink_work_queue(w)	taskqueue_enqueue(taskqueue_thread, (w))
#define	glink_work_drain(w)	taskqueue_drain(taskqueue_thread, (w))

#else /* !_KERNEL: the test on the PC */
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef pthread_mutex_t	glink_mutex_t;
typedef pthread_cond_t	glink_cond_t;
typedef unsigned int	u_int;

#define	glink_mutex_init(m)	pthread_mutex_init((m), NULL)
#define	glink_mutex_destroy(m)	pthread_mutex_destroy(m)
#define	glink_lock(m)		pthread_mutex_lock(m)
#define	glink_unlock(m)		pthread_mutex_unlock(m)
#define	glink_cond_init(c)	pthread_cond_init((c), NULL)
#define	glink_cond_destroy(c)	pthread_cond_destroy(c)
#define	glink_broadcast(c)	pthread_cond_broadcast(c)
static inline int
glink_cond_wait(glink_cond_t *c, glink_mutex_t *m, unsigned int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	return (pthread_cond_timedwait(c, m, &ts));
}
#define	glink_zalloc(n)		calloc(1, (n))
#define	glink_free(p)		free(p)
static inline uint64_t
glink_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define	glink_log(...)		fprintf(stderr, "glink: " __VA_ARGS__)
#define	glink_wmb()		__atomic_thread_fence(__ATOMIC_RELEASE)
#define	GLINK_TIMEDOUT		ETIMEDOUT

/* Deferred work: one thread per work item, woken by a semaphore */
#include <semaphore.h>
typedef struct {
	void		(*fn)(void *, int);
	void		*arg;
	sem_t		sem;
	pthread_t	thread;
	int		pending;	/* queued, not yet started */
	int		running;
	pthread_mutex_t	lock;
	pthread_cond_t	idle;
} glink_work_t;
static void *
glink_work_thread(void *p)
{
	glink_work_t *w = p;

	for (;;) {
		sem_wait(&w->sem);
		pthread_mutex_lock(&w->lock);
		if (w->pending == 0) {
			pthread_mutex_unlock(&w->lock);
			continue;
		}
		w->pending = 0;
		w->running = 1;
		pthread_mutex_unlock(&w->lock);
		w->fn(w->arg, 1);
		pthread_mutex_lock(&w->lock);
		w->running = 0;
		pthread_cond_broadcast(&w->idle);
		pthread_mutex_unlock(&w->lock);
	}
	return (NULL);
}
static inline void
glink_work_init(glink_work_t *w, void (*fn)(void *, int), void *arg)
{
	memset(w, 0, sizeof(*w));
	w->fn = fn;
	w->arg = arg;
	sem_init(&w->sem, 0, 0);
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->idle, NULL);
	pthread_create(&w->thread, NULL, glink_work_thread, w);
	pthread_detach(w->thread);
}
static inline void
glink_work_queue(glink_work_t *w)
{
	pthread_mutex_lock(&w->lock);
	if (!w->pending) {
		w->pending = 1;
		sem_post(&w->sem);
	}
	pthread_mutex_unlock(&w->lock);
}
static inline void
glink_work_drain(glink_work_t *w)
{
	pthread_mutex_lock(&w->lock);
	while (w->pending || w->running)
		pthread_cond_wait(&w->idle, &w->lock);
	pthread_mutex_unlock(&w->lock);
}
#ifndef roundup2
#define	roundup2(x, y)		(((x) + ((y) - 1)) & ~((y) - 1))
#endif
#ifndef MIN
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#endif
#ifndef nitems
#define	nitems(x)		(sizeof((x)) / sizeof((x)[0]))
#endif
#endif /* _KERNEL */

#endif /* _GLINK_OS_H_ */
