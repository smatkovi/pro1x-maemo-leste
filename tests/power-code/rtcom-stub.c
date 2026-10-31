/* PC stub of rtcom-eventlogger for wake-up measurements: no database */
#include <glib-object.h>
typedef struct _RTComEl RTComEl;
typedef struct _RTComElQuery RTComElQuery;
typedef struct _RTComElIter RTComElIter;
RTComEl *rtcom_el_new(void) { return NULL; }
RTComElQuery *rtcom_el_query_new(RTComEl *e) { (void)e; return NULL; }
void rtcom_el_query_set_limit(RTComElQuery *q, gint l) { (void)q; (void)l; }
gboolean rtcom_el_query_prepare(RTComElQuery *q, ...) { (void)q; return FALSE; }
RTComElIter *rtcom_el_get_events(RTComEl *e, RTComElQuery *q) { (void)e; (void)q; return NULL; }
gboolean rtcom_el_iter_first(RTComElIter *i) { (void)i; return FALSE; }
gboolean rtcom_el_iter_next(RTComElIter *i) { (void)i; return FALSE; }
gboolean rtcom_el_iter_get_values(RTComElIter *i, ...) { (void)i; return FALSE; }
gint rtcom_el_set_read_event(RTComEl *e, gint id, gboolean r, GError **err) { (void)e; (void)id; (void)r; (void)err; return 0; }
