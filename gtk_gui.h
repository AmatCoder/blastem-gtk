#ifndef GTK_GUI_H_
#define GTK_GUI_H_

#ifdef GTK2
  #include <gtk/gtk.h>
#else
  #include <gtk/gtkx.h>
#endif

extern GtkWidget* topwindow;
extern GtkWidget* menubar;

void create_gui(unsigned long XID, int fullscreen, char* romfname, int width, int height);

#endif //GTK_GUI_H_
