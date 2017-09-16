#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include "blastem.h"
#include "render.h"

GtkWidget* topwindow;
GtkWidget *menubar;

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  return FALSE;
}

void quit_gui(GtkMenuItem *menuitem, gpointer data)
{
  gtk_widget_destroy(GTK_WIDGET(data));
}

void set_fs(GtkMenuItem *menuitem, gpointer data)
{
  render_toggle_fullscreen();
}

void show_chooser(GtkMenuItem *menuitem, gpointer data)
{
  GtkWidget *exe;
  GtkWidget *dialog;
  char* rom = NULL;

  dialog = gtk_file_chooser_dialog_new(
    "Choose a ROM...", GTK_WINDOW(gtk_widget_get_toplevel(data)),
    GTK_FILE_CHOOSER_ACTION_OPEN, ("_Cancel"),
    GTK_RESPONSE_CANCEL, ("_Open"), GTK_RESPONSE_ACCEPT,
    NULL);

  //GtkFileFilter* filter = gtk_file_filter_new();
  //gtk_file_filter_add_pattern(filter, "*.*");
  //gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(exe), filter);


  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    rom = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));
  }

  gtk_widget_destroy (dialog);

  if (rom)
    load(rom);
}

void create_gui(unsigned long XID, int fullscreen, char* romfname, int width, int height)
{
  GtkWidget *socket;

  GtkWidget *vbox;
  GtkWidget *fileMenu;
  GtkWidget *viewMenu;
  GtkWidget *file;
  GtkWidget *view;
  GtkWidget *open;
  GtkWidget *quit;
  GtkWidget *fs;

  gtk_init(NULL, NULL);

  topwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  socket = gtk_socket_new();
  vbox = gtk_vbox_new(FALSE, 0);

  menubar = gtk_menu_bar_new();
  fileMenu = gtk_menu_new();
  viewMenu = gtk_menu_new();

  file = gtk_menu_item_new_with_label("File");
  open = gtk_menu_item_new_with_label("Open ROM...");
  quit = gtk_menu_item_new_with_label("Quit");

  view = gtk_menu_item_new_with_label("View");
  fs = gtk_menu_item_new_with_label("FullScreen");

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), viewMenu);

  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quit);
  gtk_menu_shell_append(GTK_MENU_SHELL(viewMenu), fs);

  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view);

  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(topwindow), vbox);
  gtk_container_add(GTK_CONTAINER(vbox), socket);

  g_signal_connect(topwindow, "delete-event", G_CALLBACK(delete_event), NULL);
  g_signal_connect(quit, "activate", G_CALLBACK(quit_gui), topwindow);
  g_signal_connect(open, "activate", G_CALLBACK(show_chooser), socket);
  g_signal_connect(fs, "activate", G_CALLBACK(set_fs), NULL);

  if (height <= 0) {
    float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
    height = ((float)width / aspect) + 0.5f;
  }
  gtk_widget_set_size_request(socket, width, height);


  gtk_socket_add_id(GTK_SOCKET(socket), XID);

  gtk_window_set_title(GTK_WINDOW(topwindow), "BlastEm");
  gtk_widget_show_all(topwindow);

  if (fullscreen)
    set_fs(NULL, NULL); 

  if (romfname)
    load(romfname);
}
