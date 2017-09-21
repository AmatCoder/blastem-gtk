#ifndef GTK_GUI_H_
#define GTK_GUI_H_

#ifdef GTK2
  #include <gtk/gtk.h>
#else
  #include <gtk/gtkx.h>
#endif

extern GtkWidget* topwindow;

void gui_toggle_fullscreen(GObject *object, int is_fullscreen);
void enable_menus(GObject *object);
void create_gui(unsigned long XID, int fullscreen, int width, int height);

#endif //GTK_GUI_H_
