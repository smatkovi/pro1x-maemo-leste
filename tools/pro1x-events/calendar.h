/*
 * The Events page's appointments (pro1x-events.c): what the calendar of
 * Leste's and FreeBSD's Hildon (qalendar) keeps, through its library,
 * calendar-backend, whose API is C++ (calendar.cpp).
 */
#ifndef PRO1X_EVENTS_CALENDAR_H
#define PRO1X_EVENTS_CALENDAR_H

#include <glib.h>

G_BEGIN_DECLS

enum { CAL_EVENT, CAL_TODO, CAL_BIRTHDAY };

typedef struct {
	char *summary, *location;
	gint64 start, end;	/* of this instance, Unix time */
	gboolean allday;
	int kind;		/* CAL_* */
	int colour;		/* its calendar's (calendar-backend's CalendarColour) */
	int born;		/* a birthday's year, else 0 */
	char *id;		/* the component's, */
	int calendar_id;	/* in this calendar (qalendar's launch_view) */
} CalEntry;

/* The instances of the visible calendars' events and birthdays and their
 * open tasks between FROM and TO (Unix time), as qalendar's agenda finds
 * them, by their start; at most LIMIT */
GPtrArray *pro1x_calendar_load(gint64 from, gint64 to, guint limit);

G_END_DECLS

#endif
