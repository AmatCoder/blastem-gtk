#ifndef GTK_GUI_H_
#define GTK_GUI_H_

#ifdef GTK2
  #include <gtk/gtk.h>
#else
  #include <gtk/gtkx.h>
#endif

#ifdef G_OS_WIN32
	#include <windows.h>
	typedef HWND NativeWindow;
#else
	typedef unsigned long NativeWindow;
#endif

extern GtkWidget* topwindow;

void set_default_speed();
void gui_toggle_fullscreen(GObject *object, int is_fullscreen);
void enable_menus(GObject *object);
void create_gui(NativeWindow XID, int fullscreen, int width, int height);

#endif //GTK_GUI_H_
