/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The FM app's FreeBSD backend (apps/pro1x-fm/pro1x-fm.c without
 * __linux__: requests to pro1x-fmd's socket) against the daemon and the
 * simulated chip of run.sh, without its window: the app's file is
 * included with its main renamed, libhildon's calls are stubs.
 */
#define main app_main
#include "../../../apps/pro1x-fm/pro1x-fm.c"
#undef main

void hildon_gtk_init(int *a, char ***b) { (void)a, (void)b; }
GtkWidget *hildon_stackable_window_new(void) { return NULL; }
GtkWidget *hildon_app_menu_new(void) { return NULL; }
void hildon_app_menu_append(HildonAppMenu *m, GtkButton *b) { (void)m, (void)b; }
void hildon_window_set_app_menu(GtkWindow *w, HildonAppMenu *m) { (void)w, (void)m; }
GtkWidget *hildon_gtk_button_new(HildonSizeType t) { (void)t; return NULL; }
GtkWidget *hildon_gtk_toggle_button_new(HildonSizeType t) { (void)t; return NULL; }
void hildon_helper_set_logical_font(GtkWidget *w, const char *f) { (void)w, (void)f; }
void hildon_helper_set_logical_color(GtkWidget *w, GtkRcFlags f, GtkStateType s,
    const char *c) { (void)w, (void)f, (void)s, (void)c; }
GtkWidget *hildon_banner_show_information(GtkWidget *w, const char *i,
    const char *t) { (void)w, (void)i, (void)t; return NULL; }
GtkWidget *hildon_touch_selector_new_text(void) { return NULL; }
void hildon_touch_selector_append_text(GtkWidget *w, const char *t) { (void)w, (void)t; }
int hildon_touch_selector_get_active(GtkWidget *w, int c) { (void)w, (void)c; return -1; }
GtkWidget *hildon_picker_dialog_new(GtkWindow *w) { (void)w; return NULL; }
void hildon_picker_dialog_set_selector(GtkWidget *d, GtkWidget *s) { (void)d, (void)s; }

static int failures;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); \
	failures++; } } while (0)

int
main(void)
{
	struct result r;

	CHECK(radio_open() == 0);
	CHECK(set_freq(99900) == 0);
	usleep(800000);
	memset(&r, 0, sizeof(r));
	status(&r);
	printf("app: %u kHz, signal %u %%, %s%s, \"%s\", \"%s\"\n", r.freq,
	    r.signal, r.stereo ? "stereo" : "mono", r.rds ? " RDS" : "",
	    r.ps, r.rt);
	CHECK(r.err == 0 && r.freq == 99900 && r.signal == 100 && r.stereo &&
	    r.rds && strcmp(r.ps, "TESTFM") == 0 &&
	    strcmp(r.rt, "Hello from the fake WCN3950") == 0);
	CHECK(seek(1) == 0);
	memset(&r, 0, sizeof(r));
	status(&r);
	CHECK(r.freq == 103800);
	CHECK(set_mute(1) == 0 && set_mute(0) == 0);
	radio_close();
	CHECK(fd == -1);
	/* the daemon gone: the app says so */
	CHECK(fmd("status", NULL, 0) == ENOENT);
	printf("%s: %d failures\n", failures ? "FAIL" : "ok", failures);
	return failures != 0;
}
