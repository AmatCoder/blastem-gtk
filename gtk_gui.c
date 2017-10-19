#include <gtk/gtk.h>

#ifndef GTK2
  #include <gtk/gtkx.h>
#endif

#ifdef G_OS_WIN32
#include "gdk/gdkwin32.h"
#endif

#include "blastem.h"
#include "render.h"

GtkWidget* topwindow;

void show_about(GtkMenuItem *menuitem, gpointer data)
{
  const char *authors[3] = {"Mike Pavone (Emulator)", "AmatCoder (GUI)", NULL};

  const char *gpl3 =
"blastem-gtk is free software: you can redistribute it and/or modifyit \
under the terms of the GNU General Public License as published by \
the Free Software Foundation, either version 3 of the License, or \
(at your option) any later version.\n\n\
blastem-gtk is distributed in the hope that it will be useful, \
but WITHOUT ANY WARRANTY; without even the implied warranty of \
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the \
GNU General Public License for more details.\n\n\
You should have received a copy of the GNU General Public License \
along with blastem-gtk.  If not, see <http://www.gnu.org/licenses/>.";


  gtk_show_about_dialog(GTK_WINDOW(data),
  "program-name", "blastem-gtk",
  "version" , "0.5.2-pre",
  "authors", authors,
  "copyright", "Copyright \xc2\xa9 2013-2017 Michael Pavone\nCopyright \xc2\xa9 2017 AmatCoder",
  "comments", "A GTK+ port of BlastEm emulator",
  "license", gpl3,
  "wrap-license", TRUE,
  "website", "https://github.com/AmatCoder/blastem-gtk",
  "logo", gtk_window_get_icon(GTK_WINDOW(data)),
  NULL);
}

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

  if (running)
    current_system->request_exit(current_system);
  running = 1;
}

void set_default_speed()
{
  GtkWidget *speed = g_object_get_data(G_OBJECT(topwindow), "default_speed");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(speed), TRUE);
}

void soft_reset(GtkMenuItem *menuitem, gpointer data)
{
    if (running)
      current_system->soft_reset(current_system);
}

void reloadmedia(GtkMenuItem *menuitem, gpointer data)
{
    if (running)
    {
      set_default_speed();
      reload_media();
    }
}

void set_scanlines(GtkMenuItem *menuitem, gpointer data)
{
  scanlines = !scanlines;
}

void set_fullscreen(GtkMenuItem *menuitem, gpointer data)
{
  if (running)
    render_toggle_fullscreen();
}

void save_screen(GtkMenuItem *menuitem, gpointer data)
{
  GtkWidget *dialog;
  char* path = NULL;

  dialog = gtk_file_chooser_dialog_new(
    "Save screenshot...", GTK_WINDOW(data),
    GTK_FILE_CHOOSER_ACTION_SAVE, ("_Cancel"),
    GTK_RESPONSE_CANCEL, ("_Save"), GTK_RESPONSE_ACCEPT,
    NULL);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER(dialog), TRUE);
  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER(dialog), "untitled.ppm");

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));


  gtk_widget_destroy (dialog);
  render_save_screenshot(path);
}

char* get_file_chooser(gpointer data)
{
#ifdef G_OS_WIN32
  OPENFILENAME ofn;
  char szFile[MAX_PATH] = "";

  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = NULL  ;
  ofn.lpstrFile = szFile ;
  ofn.lpstrFile[0] = '\0';
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = "All\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrFileTitle = NULL ;
  ofn.nMaxFileTitle = 0 ;
  ofn.lpstrInitialDir = NULL ;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST ;

  if(GetOpenFileName(&ofn))
  {
    char *result = (char *)malloc(MAX_PATH+1);
    strcpy(result, szFile);
    return result;
  }
  else return NULL;
#else
  GtkWidget *dialog;
  char* rom = NULL;

  dialog = gtk_file_chooser_dialog_new(
    "Choose a file...", GTK_WINDOW(data),
    GTK_FILE_CHOOSER_ACTION_OPEN, ("_Cancel"),
    GTK_RESPONSE_CANCEL, ("_Open"), GTK_RESPONSE_ACCEPT,
    NULL);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    rom = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));

  gtk_widget_destroy (dialog);
  return rom;
#endif
}

void open_rom(GtkMenuItem *menuitem, gpointer data)
{
  char* rom = get_file_chooser(data);

  if (rom)
  {
    g_object_set_data(G_OBJECT(topwindow), "rom", rom);
    if (running)
    {
      set_default_speed();
      reset_savestate();
      current_system->next_rom = g_strdup(rom);
      current_system->request_exit(current_system);
    }
    else load(rom);
  }
}

void gui_load_state(GtkMenuItem *menuitem, gpointer data)
{
  GtkWidget *dialog;
  char *statefile = get_file_chooser(data);

  if (statefile)
  {
    set_default_speed();
    load_savestate(g_strdup(g_object_get_data(G_OBJECT(topwindow), "rom")), statefile);
  }
}

void gui_save_state(GtkMenuItem *menuitem, gpointer data)
{
  GtkWidget *dialog;
  char* path = save_state_path;

  dialog = gtk_file_chooser_dialog_new(
    "Save state file...", GTK_WINDOW(data),
    GTK_FILE_CHOOSER_ACTION_SAVE, ("_Cancel"),
    GTK_RESPONSE_CANCEL, ("_Save"), GTK_RESPONSE_ACCEPT,
    NULL);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER(dialog), TRUE);
  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER(dialog), "untitled.state");

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    save_state_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));

  current_system->save_state = QUICK_SAVE_SLOT+1;
  gtk_widget_destroy (dialog);
}

void set_speed(GtkMenuItem *menuitem, gpointer data)
{
  if (!running)
    return;

  if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem)))
  {
    gpointer *speed = g_object_get_data(G_OBJECT(menuitem), "speed");
    current_system->set_speed_percent(current_system, GPOINTER_TO_UINT(speed));
  }
}

GtkWidget* menu_radio_new(GtkWidget *menu, GSList **menu_list, GSList **radio_group, guint label)
{
  GtkWidget *widget;
  gchar* f_label;

  f_label = g_strdup_printf("%i%%", label);
  widget = gtk_radio_menu_item_new_with_label(*radio_group, f_label);
  g_free(f_label);

  g_object_set_data(G_OBJECT(widget), "speed", GUINT_TO_POINTER(label));

  *menu_list = g_slist_append(*menu_list, widget);
  gtk_widget_set_sensitive(widget, FALSE);

  *radio_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(widget));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), widget);
  g_signal_connect(widget, "activate", G_CALLBACK(set_speed), NULL);

  return widget;
}

GtkWidget* menu_disable_new(GSList **menu_list, const gchar *label)
{
  GtkWidget* widget;
  widget = gtk_menu_item_new_with_label(label);
  gtk_widget_set_sensitive(widget, FALSE);
  *menu_list = g_slist_append(*menu_list, widget);

  return widget;
}

void create_gui(NativeWindow XID, int fullscreen, int width, int height)
{
  GtkWidget *socket;
  GtkWidget *vbox;
  GtkWidget *menubar;

  GtkWidget *fileMenu;
  GtkWidget *systemMenu;
  GtkWidget *videoMenu;
  GtkWidget *speedMenu;
  GtkWidget *helpMenu;

  GtkWidget *file;
  GtkWidget *open;
  GtkWidget *quit;

  GtkWidget *system;
  GtkWidget *softReset;
  GtkWidget *reloadMedia;
  GtkWidget *loadState;
  GtkWidget *saveState;
  GtkWidget *setSpeed;
  GtkWidget *setSpeed0;
  GtkWidget *setSpeed1;
  GtkWidget *setSpeed2;
  GtkWidget *setSpeed3;
  GtkWidget *setSpeed4;
  GtkWidget *setSpeed5;
  GtkWidget *setSpeed6;
  GtkWidget *setSpeed7;

  GtkWidget *video;
  GtkWidget *fullScreen;
  GtkWidget *scanLines;
  GtkWidget *saveScreen;

  GtkWidget *help;
  GtkWidget *about;

  GSList *menu_list = NULL;
  GSList *speed_list = NULL;

  gtk_init(NULL, NULL);

  topwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifndef G_OS_WIN32
  socket = gtk_socket_new();
#endif
  vbox = gtk_vbox_new(FALSE, 0);

  menubar = gtk_menu_bar_new();
  fileMenu = gtk_menu_new();
  systemMenu = gtk_menu_new();
  videoMenu = gtk_menu_new();
  speedMenu = gtk_menu_new();
  helpMenu = gtk_menu_new();

  file = gtk_menu_item_new_with_label("File");
  open = gtk_menu_item_new_with_label("Open ROM...");
  quit = gtk_menu_item_new_with_label("Quit");

  system = gtk_menu_item_new_with_label("System");
  softReset = menu_disable_new(&menu_list, "Soft Reset");
  reloadMedia= menu_disable_new(&menu_list, "Reload");
  setSpeed = gtk_menu_item_new_with_label("Speed");
  loadState = menu_disable_new(&menu_list, "Load State");
  saveState = menu_disable_new(&menu_list, "Save State");

  video = gtk_menu_item_new_with_label("Video");
  fullScreen = menu_disable_new(&menu_list, "FullScreen");
  scanLines = gtk_check_menu_item_new_with_label("Scanlines");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(scanLines), scanlines);
  saveScreen = menu_disable_new(&menu_list, "Save Screenshot");

  help = gtk_menu_item_new_with_label("Help");
  about = gtk_menu_item_new_with_label("About");

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), fileMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(system), systemMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(setSpeed), speedMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(video), videoMenu);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(help), helpMenu);

  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), open);
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(GTK_MENU_SHELL(fileMenu), quit);

  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), softReset);
  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), reloadMedia);
  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), loadState);
  gtk_menu_shell_append(GTK_MENU_SHELL(systemMenu), saveState);

  setSpeed5 = menu_radio_new(speedMenu, &menu_list, &speed_list, 25);
  setSpeed6 = menu_radio_new(speedMenu, &menu_list, &speed_list, 50);
  setSpeed7 = menu_radio_new(speedMenu, &menu_list, &speed_list, 75);
  setSpeed0 = menu_radio_new(speedMenu, &menu_list, &speed_list, 100);
  setSpeed1 = menu_radio_new(speedMenu, &menu_list, &speed_list, 150);
  setSpeed2 = menu_radio_new(speedMenu, &menu_list ,&speed_list, 200);
  setSpeed3 = menu_radio_new(speedMenu, &menu_list ,&speed_list, 300);
  setSpeed4 = menu_radio_new(speedMenu, &menu_list ,&speed_list, 400);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(setSpeed0), TRUE);
  g_object_set_data(G_OBJECT(topwindow), "default_speed", setSpeed0);

  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), fullScreen);
  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), scanLines);
  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(GTK_MENU_SHELL(videoMenu), saveScreen);

  gtk_menu_shell_append(GTK_MENU_SHELL(helpMenu), about);

  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), system);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), setSpeed);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), video);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help);

  gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(topwindow), vbox);
#ifndef G_OS_WIN32
  gtk_container_add(GTK_CONTAINER(vbox), socket);
#endif

  g_signal_connect(topwindow, "delete-event", G_CALLBACK(delete_event), NULL);
  g_signal_connect(quit, "activate", G_CALLBACK(quit_gui), topwindow);
  g_signal_connect(open, "activate", G_CALLBACK(open_rom), topwindow);
  g_signal_connect(fullScreen, "activate", G_CALLBACK(set_fullscreen), NULL);
  g_signal_connect(scanLines, "activate", G_CALLBACK(set_scanlines), NULL);
  g_signal_connect(softReset, "activate", G_CALLBACK(soft_reset), NULL);
  g_signal_connect(reloadMedia, "activate", G_CALLBACK(reloadmedia), NULL);
  g_signal_connect(saveScreen, "activate", G_CALLBACK(save_screen), topwindow);
  g_signal_connect(loadState, "activate", G_CALLBACK(gui_load_state), topwindow);
  g_signal_connect(saveState, "activate", G_CALLBACK(gui_save_state), topwindow);
  g_signal_connect(about, "activate", G_CALLBACK(show_about), topwindow);

  g_object_set_data_full(G_OBJECT(topwindow), "menu_list", menu_list, (GDestroyNotify) g_list_free);
  g_object_set_data(G_OBJECT(topwindow), "menubar", menubar);

  if (height <= 0) {
    float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
    height = ((float)width / aspect) + 0.5f;
  }

#ifdef G_OS_WIN32
  gtk_widget_set_size_request(menubar, 2048, 25);
  gtk_widget_show_all(topwindow);
  GdkWindow *gdkWindow = gdk_win32_window_foreign_new_for_display(gdk_screen_get_display(gdk_screen_get_default()), XID);
  gdk_window_reparent(gtk_widget_get_window(menubar), gdkWindow, 0 , 0);
  gtk_widget_hide(topwindow);
  gdk_window_focus (gdkWindow, 0);
#else
  gtk_widget_set_size_request(socket, width, height);
  gtk_socket_add_id(GTK_SOCKET(socket), XID);
  gtk_window_set_title(GTK_WINDOW(topwindow), "BlastEm");
  gtk_window_set_icon(GTK_WINDOW(topwindow), gdk_pixbuf_new_from_resource("/org/blastem-gtk/icons/logo-gtk.png", NULL));
  gtk_widget_show_all(topwindow);
#endif

  if (fullscreen)
    set_fullscreen(NULL, NULL);
}
