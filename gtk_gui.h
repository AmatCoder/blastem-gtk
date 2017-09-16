#ifndef GTK_GUI_H_
#define GTK_GUI_H_

#include <gtk/gtkx.h>

extern GtkWidget* topwindow;
extern GtkWidget* menubar;

void create_gui(unsigned long XID, int fullscreen, char* romfname, int width, int height);

#endif //GTK_GUI_H_
