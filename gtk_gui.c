#include <gtk/gtk.h>

#ifndef GTK2
  #include <gtk/gtkx.h>
#endif

#include "blastem.h"
#include "render.h"

GtkWidget* topwindow;

void gui_toggle_fullscreen(GObject *object, int is_fullscreen)
{
  GtkWidget *menubar = g_object_get_data(object, "menubar");
  if (is_fullscreen)
  {
    gtk_widget_hide(menubar);
    gtk_window_fullscreen(GTK_WINDOW(object));
  }
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(object));
    gtk_widget_show(menubar);
  }
}

void enable_menus(GObject *object)
{
  GSList *list = g_object_get_data(object, "menu_list");
  GSList *l;
  for (l = list; l != NULL; l = l->next)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(l->data), TRUE);
  }
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  running = 1;
  return FALSE;
}

void quit_gui(GtkMenuItem *menuitem, gpointer data)
{
  gtk_widget_destroy(GTK_WIDGET(data));
  running = 1;
}

void soft_reset(GtkMenuItem *menuitem, gpointer data)
{
    if (running)
      current_system->soft_reset(current_system);
}

void reloadmedia(GtkMenuItem *menuitem, gpointer data)
{
    if (running)
      reload_media();
}

void set_scanlines(GtkMenuItem *menuitem, gpointer data)
{
    scanlines = !scanlines;
}

void set_fullscreen(GtkMenuItem *menuitem, gpointer data)
{
  if (running == 1)
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
  {
    if (running)
    {
      current_system->next_rom = rom;
      current_system->request_exit(current_system);
    }
    else load(rom);
  }
}

GtkWidget* menu_disable_new(GSList **menu_list, const gchar *label)
{
  GtkWidget* widget;
  widget = gtk_menu_item_new_with_label(label);
  gtk_widget_set_sensitive(widget, FALSE);
  *menu_list = g_slist_append(*menu_list, widget);

  return widget;
}

void create_gui(unsigned long XID, int fullscreen, int width, int height)
{
  GtkWidget *socket;
  GtkWidget *vbox;
  GtkWidget *menubar;

  GtkWidget *fileMenu;
  GtkWidget *systemMenu;
  GtkWidget *videoMenu;

  GtkWidget *file;
  GtkWidget *open;
  GtkWidget *quit;

  GtkWidget *system;
  GtkWidget *softReset;
  GtkWidget *reloadMedia;

  GtkWidget *video;
  GtkWidget *fullScreen;
  GtkWidget *scanLines;

  GSList *menu_list = NULL;

  gtk_init(NULL, NULL);

  topwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  socket = gtk_socket_new();
  vbox = gtk_vbox_new(FALSE, 0);

  menubar = gtk_menu_bar_new();
  fileMenu = gtk_menu_new();
  systemMenu = gtk_menu_new();
  videoMenu = gtk_menu_new();

  file = gtk_menu_item_new_with_label("File");
  open = gtk_menu_item_new_with_label("Open ROM...");
  quit = gtk_menu_item_new_with_label("Quit");

  system = gtk_menu_item_new_with_label("System");
  softReset = menu_disable_new(&menu_list, "Soft Reset");
  reloadMedia= menu_disable_new(&menu_list, "Reload Media");

  video = gtk_menu_item_new_with_label("Video");
  fullScreen = menu_disable_new(&menu_list, "FullScreen");
  scanLines = gtk_check_menu_item_new_with_label("Scanlines");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(scanLines), scanlines);

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(system), systemMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(video), videoMenu);

  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new ());
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quit);

  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), softReset);
  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), reloadMedia);

  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), fullScreen);
  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), scanLines);

  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), system);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), video);

  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(topwindow), vbox);
  gtk_container_add(GTK_CONTAINER(vbox), socket);

  g_signal_connect(topwindow, "delete-event", G_CALLBACK(delete_event), NULL);
  g_signal_connect(quit, "activate", G_CALLBACK(quit_gui), topwindow);
  g_signal_connect(open, "activate", G_CALLBACK(show_chooser), socket);
  g_signal_connect(fullScreen, "activate", G_CALLBACK(set_fullscreen), NULL);
  g_signal_connect(scanLines, "activate", G_CALLBACK(set_scanlines), NULL);
  g_signal_connect(softReset, "activate", G_CALLBACK(soft_reset), NULL);
  g_signal_connect(reloadMedia, "activate", G_CALLBACK(reloadmedia), NULL);

  g_object_set_data_full(G_OBJECT(topwindow), "menu_list", menu_list, (GDestroyNotify) g_list_free);
  g_object_set_data(G_OBJECT(topwindow), "menubar", menubar);

  if (height <= 0) {
    float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
    height = ((float)width / aspect) + 0.5f;
  }
  gtk_widget_set_size_request(socket, width, height);


  gtk_socket_add_id(GTK_SOCKET(socket), XID);

  gtk_window_set_title(GTK_WINDOW(topwindow), "BlastEm");
  gtk_widget_show_all(topwindow);

  if (fullscreen)
    set_fullscreen(NULL, NULL);
}
