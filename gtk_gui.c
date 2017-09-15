#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include "blastem.h"

GtkWidget* topwindow;

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  gtk_main_quit();

  return FALSE;
}

void quit_gui(GtkMenuItem *menuitem, gpointer data)
{
  gtk_widget_destroy(GTK_WIDGET(data));

  gtk_main_quit();
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

  gtk_widget_hide(data);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    rom = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));
  }

  gtk_widget_destroy (dialog);

  gtk_widget_show(data);

  gtk_widget_set_can_focus (data, TRUE);
  gtk_widget_grab_focus (data);
  gtk_widget_set_can_focus (data, FALSE);

  if (rom)
    load(rom);
}

void create_gui(unsigned long XID, int width, int height)
{
  GtkWidget *frame;
  GtkWidget *socket;

  GtkWidget *vbox;
  GtkWidget *menubar;
  GtkWidget *fileMenu;
  GtkWidget *file;
  GtkWidget *open;
  GtkWidget *quit;

  gtk_init(NULL, NULL);

  topwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  frame = gtk_frame_new (NULL);
  socket = gtk_socket_new();
  vbox = gtk_vbox_new(FALSE, 0);

  menubar = gtk_menu_bar_new();
  fileMenu = gtk_menu_new();

  file = gtk_menu_item_new_with_label("File");
  open = gtk_menu_item_new_with_label("Open ROM...");
  quit = gtk_menu_item_new_with_label("Quit");

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quit);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);

  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(topwindow), vbox);
  gtk_container_add(GTK_CONTAINER(vbox), frame);
  gtk_container_add (GTK_CONTAINER (frame), socket);

  g_signal_connect(topwindow, "delete-event", G_CALLBACK(delete_event), NULL);
  g_signal_connect(quit, "activate", G_CALLBACK(quit_gui), topwindow);
  g_signal_connect(open, "activate", G_CALLBACK(show_chooser), socket);

  gtk_widget_set_size_request(socket, width, height);

  GdkRGBA bg = { 0, 0, 0, 1 };
  gtk_widget_override_background_color(frame, GTK_STATE_FLAG_NORMAL, &bg);

  gtk_socket_add_id(GTK_SOCKET(socket), XID);

  gtk_widget_show_all(topwindow);
  gtk_widget_hide(socket);

  gtk_main();
}
