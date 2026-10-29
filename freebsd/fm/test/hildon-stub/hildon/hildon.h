/* A stub of the libhildon calls apps/pro1x-fm makes, for test_app.c on the PC */
#include <gtk/gtk.h>
typedef struct _HildonAppMenu HildonAppMenu;
typedef enum { HILDON_SIZE_AUTO, HILDON_SIZE_FINGER_HEIGHT } HildonSizeType;
#define HILDON_MARGIN_DEFAULT 8
#define HILDON_MARGIN_DOUBLE 16
#define HILDON_APP_MENU(x) ((HildonAppMenu *)(x))
#define HILDON_WINDOW(x) ((GtkWindow *)(x))
#define HILDON_TOUCH_SELECTOR(x) ((GtkWidget *)(x))
#define HILDON_PICKER_DIALOG(x) ((GtkWidget *)(x))
void hildon_gtk_init(int *, char ***);
GtkWidget *hildon_stackable_window_new(void);
GtkWidget *hildon_app_menu_new(void);
void hildon_app_menu_append(HildonAppMenu *, GtkButton *);
void hildon_window_set_app_menu(GtkWindow *, HildonAppMenu *);
GtkWidget *hildon_gtk_button_new(HildonSizeType);
GtkWidget *hildon_gtk_toggle_button_new(HildonSizeType);
void hildon_helper_set_logical_font(GtkWidget *, const char *);
void hildon_helper_set_logical_color(GtkWidget *, GtkRcFlags, GtkStateType, const char *);
GtkWidget *hildon_banner_show_information(GtkWidget *, const char *, const char *);
GtkWidget *hildon_touch_selector_new_text(void);
void hildon_touch_selector_append_text(GtkWidget *, const char *);
int hildon_touch_selector_get_active(GtkWidget *, int);
GtkWidget *hildon_picker_dialog_new(GtkWindow *);
void hildon_picker_dialog_set_selector(GtkWidget *, GtkWidget *);
