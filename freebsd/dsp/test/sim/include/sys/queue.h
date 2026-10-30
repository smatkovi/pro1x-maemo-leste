/*-
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The simulation (freebsd/dsp/test/sim): FreeBSD's list macros that the
 * DSP drivers use, with FreeBSD's semantics (the C library's sys/queue.h
 * lacks several of them). Not included by any C library header.
 */

#ifndef _SIM_SYS_QUEUE_H_
#define	_SIM_SYS_QUEUE_H_

/* --- LIST ------------------------------------------------------------------------- */
#define	LIST_HEAD(name, type)	struct name { struct type *lh_first; }
#define	LIST_HEAD_INITIALIZER(head)	{ NULL }
#define	LIST_ENTRY(type)	struct { struct type *le_next; struct type **le_prev; }
#define	LIST_FIRST(head)	((head)->lh_first)
#define	LIST_NEXT(elm, field)	((elm)->field.le_next)
#define	LIST_EMPTY(head)	((head)->lh_first == NULL)
#define	LIST_INIT(head)		do { LIST_FIRST(head) = NULL; } while (0)
#define	LIST_FOREACH(var, head, field)					\
	for ((var) = LIST_FIRST(head); (var); (var) = LIST_NEXT(var, field))
#define	LIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = LIST_FIRST(head);					\
	    (var) && ((tvar) = LIST_NEXT(var, field), 1); (var) = (tvar))
#define	LIST_INSERT_HEAD(head, elm, field) do {				\
	if ((LIST_NEXT(elm, field) = LIST_FIRST(head)) != NULL)		\
		LIST_FIRST(head)->field.le_prev = &LIST_NEXT(elm, field);\
	LIST_FIRST(head) = (elm);					\
	(elm)->field.le_prev = &LIST_FIRST(head);			\
} while (0)
#define	LIST_REMOVE(elm, field) do {					\
	if (LIST_NEXT(elm, field) != NULL)				\
		LIST_NEXT(elm, field)->field.le_prev = (elm)->field.le_prev;\
	*(elm)->field.le_prev = LIST_NEXT(elm, field);			\
} while (0)

/* --- STAILQ ------------------------------------------------------------------------ */
#define	STAILQ_HEAD(name, type)						\
	struct name { struct type *stqh_first; struct type **stqh_last; }
#define	STAILQ_HEAD_INITIALIZER(head)	{ NULL, &(head).stqh_first }
#define	STAILQ_ENTRY(type)	struct { struct type *stqe_next; }
#define	STAILQ_FIRST(head)	((head)->stqh_first)
#define	STAILQ_NEXT(elm, field)	((elm)->field.stqe_next)
#define	STAILQ_EMPTY(head)	((head)->stqh_first == NULL)
#define	STAILQ_INIT(head) do {						\
	STAILQ_FIRST(head) = NULL;					\
	(head)->stqh_last = &STAILQ_FIRST(head);			\
} while (0)
#define	STAILQ_FOREACH(var, head, field)				\
	for ((var) = STAILQ_FIRST(head); (var); (var) = STAILQ_NEXT(var, field))
#define	STAILQ_INSERT_TAIL(head, elm, field) do {			\
	STAILQ_NEXT(elm, field) = NULL;					\
	*(head)->stqh_last = (elm);					\
	(head)->stqh_last = &STAILQ_NEXT(elm, field);			\
} while (0)
#define	STAILQ_REMOVE_HEAD(head, field) do {				\
	if ((STAILQ_FIRST(head) = STAILQ_NEXT(STAILQ_FIRST(head), field)) == NULL)\
		(head)->stqh_last = &STAILQ_FIRST(head);		\
} while (0)

/* --- TAILQ ------------------------------------------------------------------------- */
#define	TAILQ_HEAD(name, type)						\
	struct name { struct type *tqh_first; struct type **tqh_last; }
#define	TAILQ_HEAD_INITIALIZER(head)	{ NULL, &(head).tqh_first }
#define	TAILQ_ENTRY(type)	struct { struct type *tqe_next; struct type **tqe_prev; }
#define	TAILQ_FIRST(head)	((head)->tqh_first)
#define	TAILQ_NEXT(elm, field)	((elm)->field.tqe_next)
#define	TAILQ_EMPTY(head)	((head)->tqh_first == NULL)
#define	TAILQ_INIT(head) do {						\
	TAILQ_FIRST(head) = NULL;					\
	(head)->tqh_last = &TAILQ_FIRST(head);				\
} while (0)
#define	TAILQ_FOREACH(var, head, field)					\
	for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))
#define	TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST(head);					\
	    (var) && ((tvar) = TAILQ_NEXT(var, field), 1); (var) = (tvar))
#define	TAILQ_INSERT_TAIL(head, elm, field) do {			\
	TAILQ_NEXT(elm, field) = NULL;					\
	(elm)->field.tqe_prev = (head)->tqh_last;			\
	*(head)->tqh_last = (elm);					\
	(head)->tqh_last = &TAILQ_NEXT(elm, field);			\
} while (0)
#define	TAILQ_INSERT_HEAD(head, elm, field) do {			\
	if ((TAILQ_NEXT(elm, field) = TAILQ_FIRST(head)) != NULL)	\
		TAILQ_FIRST(head)->field.tqe_prev = &TAILQ_NEXT(elm, field);\
	else								\
		(head)->tqh_last = &TAILQ_NEXT(elm, field);		\
	TAILQ_FIRST(head) = (elm);					\
	(elm)->field.tqe_prev = &TAILQ_FIRST(head);			\
} while (0)
#define	TAILQ_REMOVE(head, elm, field) do {				\
	if (TAILQ_NEXT(elm, field) != NULL)				\
		TAILQ_NEXT(elm, field)->field.tqe_prev = (elm)->field.tqe_prev;\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = TAILQ_NEXT(elm, field);		\
} while (0)

#endif /* _SIM_SYS_QUEUE_H_ */
