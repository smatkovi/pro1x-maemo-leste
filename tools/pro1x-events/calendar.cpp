/*
 * The Events page's appointments from calendar-backend, the library of
 * Hildon's calendar (qalendar on Leste and FreeBSD), the way qalendar's
 * agenda (AgendaPlug.cpp, CWrapper.h) reads them: each visible calendar's
 * events, tasks and birthdays in the range, the repeating ones expanded to
 * their instances there. calendar-backend sends dbChange (D-Bus, session
 * bus) when they change; pro1x-events.c listens to it.
 */
#include <algorithm>
#include <vector>
#include <sqlite3.h>
#include <CCalendarDB.h>
#include <CMulticalendar.h>
#include <CCalendar.h>
#include <CComponent.h>
#include <Common.h>
#include "calendar.h"

using std::vector;

static void entry_free(gpointer p)
{
	CalEntry *e = static_cast<CalEntry *>(p);

	g_free(e->summary);
	g_free(e->location);
	g_free(e->id);
	g_free(e);
}

static void add(GPtrArray *out, CComponent *c, time_t start, time_t duration, int colour)
{
	CalEntry *e = g_new0(CalEntry, 1);
	int type = c->getType();

	e->summary = g_strdup(c->getSummary().c_str());
	e->location = g_strdup(c->getLocation().c_str());
	e->start = start;
	e->end = start + duration;
	e->allday = c->getAllDay() != 0 || type == E_TODO || type == E_BDAY;
	e->kind = type == E_TODO ? CAL_TODO : type == E_BDAY ? CAL_BIRTHDAY : CAL_EVENT;
	e->colour = colour;
	e->id = g_strdup(c->getId().c_str());
	e->calendar_id = c->getCalendarId();
	/* a birthday's status is the year of birth (qalendar's CWrapper) */
	if (type == E_BDAY && c->getStatus() > 1800)
		e->born = c->getStatus();
	g_ptr_array_add(out, e);
}

static gint by_start(gconstpointer a, gconstpointer b)
{
	const CalEntry *x = *(CalEntry *const *)a, *y = *(CalEntry *const *)b;

	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	if (x->allday != y->allday)
		return x->allday ? -1 : 1;
	return g_utf8_collate(x->summary, y->summary);
}

/* calendar-backend's SQL quotes some strings with double quotes (its
 * repetitions: Id = "21"), which an SQLite built without them (FreeBSD's,
 * SQLITE_DQS=0) takes for column names: the repeating appointments and the
 * birthdays are then not found. On again for its connection (for this
 * program only). */
static void allow_double_quotes(void)
{
#ifdef SQLITE_DBCONFIG_DQS_DML
	CCalendarDB *db = CCalendarDB::Instance();

	if (db && db->getDb()) {
		sqlite3_db_config(db->getDb(), SQLITE_DBCONFIG_DQS_DML, 1, (int *)0);
		sqlite3_db_config(db->getDb(), SQLITE_DBCONFIG_DQS_DDL, 1, (int *)0);
	}
#endif
}

extern "C" GPtrArray *pro1x_calendar_load(gint64 from, gint64 to, guint limit)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(entry_free);
	CMulticalendar *mc = CMulticalendar::MCInstance();

	if (!mc)
		return out;
	allow_double_quotes();
	vector<CCalendar *> calendars = mc->getListCalFromMc();
	for (size_t i = 0; i < calendars.size(); i++) {
		CCalendar *cal = calendars[i];
		int offset = 0, error = 0, colour;

		if (!cal->IsShown())
			continue;
		colour = cal->getCalendarColor();
		vector<CComponent *> comps = cal->getAllEventsTodos(from, to, 1024, offset, error);
		for (size_t j = 0; j < comps.size(); j++) {
			CComponent *c = comps[j];
			time_t start = c->getDateStart(), end = c->getDateEnd();
			time_t duration = end > start ? end - start : 0;

			/* tasks done */
			if (c->getType() == E_TODO && c->getStatus()) {
				delete c;
				continue;
			}
			if (c->getRecurrence()) {
				vector<time_t> stamps;

				c->getInstanceTimes(from, to, stamps);
				std::sort(stamps.begin(), stamps.end());
				/* (a rule's duplicates once, as qalendar does) */
				stamps.erase(std::unique(stamps.begin(), stamps.end()), stamps.end());
				for (size_t s = 0; s < stamps.size(); s++) {
					/* calendar-backend gives a day before the range
					 * too, and takes its ends as inclusive */
					if (stamps[s] > to || stamps[s] + duration < from)
						continue;
					if (stamps[s] + duration == from && duration != 0)
						continue;
					add(out, c, stamps[s], duration, colour);
				}
			} else if (!(end == from && duration != 0) && start <= to)
				add(out, c, start, duration, colour);
			delete c;
		}
	}
	mc->releaseListCalendars(calendars);
	g_ptr_array_sort(out, by_start);
	if (limit && out->len > limit)
		g_ptr_array_set_size(out, limit);
	return out;
}
