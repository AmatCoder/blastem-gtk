#include <gtk/gtk.h>
#include <gtk/gtkx.h>

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  return FALSE;
}

void create_gui(Window XID, int width, int height)
{
  GtkWidget *window;
  GtkWidget *socket;

  GtkWidget *vbox;
  GtkWidget *menubar;
  GtkWidget *fileMenu;
  GtkWidget *file;
  GtkWidget *quit;

  gtk_init(NULL, NULL);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  socket = gtk_socket_new();
  vbox = gtk_vbox_new(FALSE, 0);

  menubar = gtk_menu_bar_new();
  fileMenu = gtk_menu_new();

  file = gtk_menu_item_new_with_label("File");
  quit = gtk_menu_item_new_with_label("Quit");

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quit);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);

  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(window), vbox);
  gtk_container_add (GTK_CONTAINER (vbox), socket);

  g_signal_connect(window, "delete-event", G_CALLBACK(delete_event), NULL);
  g_signal_connect_swapped(G_OBJECT(quit), "activate", G_CALLBACK(gtk_widget_destroy), window);

  gtk_widget_set_size_request(socket, width, height);

  gtk_widget_show_all(window);
  gtk_socket_add_id(GTK_SOCKET(socket), XID);
}
