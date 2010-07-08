/*
 * japlay GTK user interface
 * Copyright Janne Kulmala 2010
 */
#include "ui.h"
#include "common.h"
#include "japlay.h"
#include "playlist.h"
#include "utils.h"
#include "iowatch.h"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <unistd.h>

enum {
	COL_ENTRY,
	COL_NAME,
	COL_LENGTH,
	COL_COLOR,
	NUM_COLS
};

struct song_ui_ctx {
	GtkTreeRowReference *rowref;
};

size_t ui_song_ctx_size = sizeof(struct song_ui_ctx);

static GtkWidget *main_window;
static GtkWidget *playlist_view;
static GtkWidget *power_bar;
static GtkListStore *playlist_store;
static GThread *main_thread;
static bool quit = false;
static int wake_fd;

static void lock_ui()
{
	if (g_thread_self() != main_thread)
		gdk_threads_enter();
}

static void unlock_ui()
{
	if (g_thread_self() != main_thread)
		gdk_threads_leave();
}

static const char *file_ext(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/') {
		if (filename[i - 1] == '.')
			return &filename[i];
		--i;
	}
	return NULL;
}

/* UI lock must be held */
static void set_playlist_color(struct song *song, const char *color)
{
	struct song_ui_ctx *ctx = get_song_ui_ctx(song);
	if (!ctx->rowref)
		return;

	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);

	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store), &iter, path);
	gtk_tree_path_free(path);

	gtk_list_store_set(playlist_store, &iter, COL_COLOR, color, -1);
}

static char *get_display_name(struct song *song)
{
	const char *filename = get_song_filename(song);

	if (!memcmp(filename, "http://", 7))
		return filename;
	return g_filename_to_utf8(file_base(filename), -1, NULL, NULL, NULL);
}

void ui_add_playlist(struct song *song)
{
	struct song_ui_ctx *ctx = get_song_ui_ctx(song);

	lock_ui();
	GtkTreeIter iter;
	gtk_list_store_append(playlist_store, &iter);
	char *name = get_display_name(song);
	char buf[32];
	sprintf(buf, "%d:%02d", get_song_length(song) / (1000 * 60),
		(get_song_length(song) / 1000) % 60);
	gtk_list_store_set(playlist_store, &iter, COL_ENTRY, song,
		COL_NAME, name, COL_LENGTH, buf, COL_COLOR, NULL, -1);
	free(name);

	/* store row reference */
	GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(playlist_store), &iter);
	ctx->rowref = gtk_tree_row_reference_new(GTK_TREE_MODEL(playlist_store), path);
	gtk_tree_path_free(path);
	unlock_ui();
}

void ui_remove_playlist(struct song *song)
{
	struct song_ui_ctx *ctx = get_song_ui_ctx(song);

	lock_ui();
	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);
	gtk_tree_row_reference_free(ctx->rowref);

	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store), &iter, path);
	gtk_tree_path_free(path);

	gtk_list_store_remove(playlist_store, &iter);
	unlock_ui();

	ctx->rowref = NULL;
}

void ui_update_playlist(struct song *song)
{
	struct song_ui_ctx *ctx = get_song_ui_ctx(song);
	if (!ctx->rowref)
		return;

	lock_ui();
	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);

	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store), &iter, path);
	gtk_tree_path_free(path);

	char *name = get_display_name(song);
	char buf[32];
	sprintf(buf, "%d:%02d", get_song_length(song) / (1000 * 60),
		(get_song_length(song) / 1000) % 60);
	gtk_list_store_set(playlist_store, &iter,
		COL_NAME, name, COL_LENGTH, buf, -1);
	free(name);
	unlock_ui();
}

void ui_set_cursor(struct song *prev, struct song *song)
{
	lock_ui();
	if (prev)
		set_playlist_color(prev, NULL);

	set_playlist_color(song, "red");

	char *name = get_display_name(song);
	char *buf = concat_strings(name, " - " APP_NAME);
	if (buf) {
		gtk_window_set_title(GTK_WINDOW(main_window), buf);
		free(buf);
	}
	free(name);

	write(wake_fd, "", 1);
	unlock_ui();
}

void ui_set_status(int power, unsigned int position)
{
	lock_ui();
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(power_bar), power / 256.0);
	char buf[10];
	sprintf(buf, "%d:%02d", position / (1000 * 60), (position / 1000) % 60);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(power_bar), buf);
	write(wake_fd, "", 1);
	unlock_ui();
}

static void add_one_file(char *filename, gpointer ptr)
{
	UNUSED(ptr);
	const char *ext = file_ext(filename);
	if (ext) {
		if (!strcasecmp(ext, "pls"))
			load_playlist_pls(filename);
		else if (!strcasecmp(ext, "m3u"))
			load_playlist_m3u(filename);
		else
			add_file_playlist(filename);
	} else
		add_file_playlist(filename);
	g_free(filename);
}

static void open_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Add Files",
		GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_OPEN,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), true);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		GSList *filelist = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
		g_slist_foreach(filelist, (GFunc)add_one_file, NULL);
		g_slist_free(filelist);
	}
	gtk_widget_destroy(dialog);
}

static void clear_playlist_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	UNUSED(menuitem);
	UNUSED(ptr);
	clear_playlist();
}

static void shuffle_playlist_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	UNUSED(menuitem);
	UNUSED(ptr);
	shuffle_playlist();
}

static void append_rr_list(GtkTreePath *path, GList **rowref_list)
{
	GtkTreeRowReference *rowref = gtk_tree_row_reference_new(
		GTK_TREE_MODEL(playlist_store), path);
	*rowref_list = g_list_prepend(*rowref_list, rowref);
	gtk_tree_path_free(path);
}

static struct song *entry_from_store(GtkTreePath *path)
{
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_store), &iter, path);

	GValue value;
	memset(&value, 0, sizeof(value));
	gtk_tree_model_get_value(GTK_TREE_MODEL(playlist_store), &iter, COL_ENTRY, &value);
	struct song *song = g_value_get_pointer(&value);
	g_value_unset(&value);

	return song;
}

static void remove_one_file(GtkTreeRowReference *rowref, gpointer ptr)
{
	UNUSED(ptr);
	GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
	gtk_tree_row_reference_free(rowref);

	struct song *song = entry_from_store(path);
	gtk_tree_path_free(path);

	remove_playlist(song);
}

static void delete_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	GList *selected = gtk_tree_selection_get_selected_rows(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view)), NULL);
	GList *rr_list = NULL;
	g_list_foreach(selected, (GFunc)append_rr_list, &rr_list);
	g_list_free(selected);
	g_list_foreach(rr_list, (GFunc)remove_one_file, NULL);
	g_list_free(rr_list);
}

static void play_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	japlay_play();
}

static void pause_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	japlay_pause();
}

static void next_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	japlay_skip();
}

static void stop_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	japlay_stop();
}

static void playlist_clicked(GtkTreeView *view, GtkTreePath *path,
	GtkTreeViewColumn *col, gpointer ptr)
{
	UNUSED(view);
	UNUSED(col);
	UNUSED(ptr);
	play_playlist(entry_from_store(path));
}

static void destroy_cb(GtkWidget *widget, gpointer ptr)
{
	UNUSED(widget);
	UNUSED(ptr);
	quit = true;
}

static int incoming_wake(int fd, int flags, void *ctx)
{
	UNUSED(flags);
	UNUSED(ctx);

	/* drain pipe read buffer */
	char buf[64];
	read(fd, buf, sizeof buf);
	return 0;
}

static gboolean key_pressed_cb(GtkWidget *widget, GdkEventKey *key, gpointer data)
{
	UNUSED(widget);
	UNUSED(data);
	switch (key->keyval) {
	case GDK_Left:
		japlay_seek_relative(-10000);
		return TRUE;
	case GDK_Right:
		japlay_seek_relative(10000);
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static int incoming_x11_event(int fd, int flags, void *ctx)
{
	UNUSED(fd);
	UNUSED(flags);
	UNUSED(ctx);
	return 0;
}

static void handle_sigint(int sig)
{
	UNUSED(sig);
	quit = true;
}

int main(int argc, char **argv)
{
	g_thread_init(NULL);
	gdk_threads_init();

	main_thread = g_thread_self();

	int fd = japlay_connect();
	if (fd >= 0) {
		int i;
		for (i = 1; i < argc; ++i)
			japlay_send(fd, argv[i]);
		close(fd);
		return 0;
	}

	if (japlay_init(&argc, argv)) {
		error("Can not initialize japlay\n");
		return -1;
	}

	gtk_init(&argc, &argv);

	main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
	g_signal_connect(G_OBJECT(main_window), "destroy", G_CALLBACK(destroy_cb), NULL);
	g_signal_connect(G_OBJECT(main_window), "key-press-event", G_CALLBACK(key_pressed_cb), NULL);

	playlist_store = gtk_list_store_new(NUM_COLS, G_TYPE_POINTER,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

	GtkWidget *menubar = gtk_menu_bar_new();

	GtkWidget *file_menu = gtk_menu_new();

	GtkWidget *item = gtk_menu_item_new_with_label("Clear playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(clear_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Shuffle playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(shuffle_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Quit");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(gtk_main_quit), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("File");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), file_menu);
	gtk_menu_bar_append(GTK_MENU_BAR(menubar), item);

	static const struct button {
		const char *stockid;
		void (*const cb)(GtkButton *button, gpointer ptr);
	} buttons[] = {
		{GTK_STOCK_OPEN, open_cb},
		{GTK_STOCK_MEDIA_PLAY, play_cb},
		{GTK_STOCK_MEDIA_STOP, stop_cb},
		{GTK_STOCK_MEDIA_PAUSE, pause_cb},
		{GTK_STOCK_MEDIA_NEXT, next_cb},
		{GTK_STOCK_DELETE, delete_cb},
		{NULL, NULL}
	};

	GtkWidget *toolbar = gtk_hbox_new(false, 0);
	int i;
	for (i = 0; buttons[i].stockid; ++i) {
		GtkWidget *button = gtk_button_new();
		GtkWidget *image = gtk_image_new_from_stock(buttons[i].stockid, GTK_ICON_SIZE_SMALL_TOOLBAR);
		gtk_button_set_image(GTK_BUTTON(button), image);
		g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(buttons[i].cb), NULL);
		gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);
	}

	power_bar = gtk_progress_bar_new();
	gtk_box_pack_start(GTK_BOX(toolbar), power_bar, false, true, 0);

	playlist_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlist_store));
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view)),
		GTK_SELECTION_MULTIPLE);
	g_signal_connect(G_OBJECT(playlist_view), "row-activated", G_CALLBACK(playlist_clicked), NULL);

	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(playlist_view),
		-1, "Name", renderer, "text", COL_NAME,
		"foreground", COL_COLOR, NULL);

	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(playlist_view),
		-1, "Length", renderer, "text", COL_LENGTH,
		"foreground", COL_COLOR, NULL);

	GtkWidget *scrollwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollwin),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scrollwin), playlist_view);

	GtkWidget *vbox = gtk_vbox_new(false, 0);
	gtk_box_pack_start(GTK_BOX(vbox), menubar, false, true, 0);
	gtk_box_pack_start(GTK_BOX(vbox), toolbar, false, true, 0);
	gtk_box_pack_start(GTK_BOX(vbox), scrollwin, true, true, 0);

	gtk_container_add(GTK_CONTAINER(main_window), vbox);
	gtk_widget_set_size_request(vbox, 350, 400);
	gtk_widget_show_all(main_window);

	char *playlistpath = get_config_name("playlist_store.m3u");
	if (playlistpath)
		load_playlist_m3u(playlistpath);

	for (i = 1; i < argc; ++i)
		add_file_playlist(argv[i]);

	int pipefd[2];
	pipe(pipefd);
	wake_fd = pipefd[1];

	new_io_watch(pipefd[0], IO_IN, incoming_wake, NULL);
	new_io_watch(ConnectionNumber(GDK_DISPLAY()), IO_IN, incoming_x11_event, NULL);

	signal(SIGINT, handle_sigint);

	while (!quit) {
		gdk_threads_enter();

		/* process events before going to sleep */
		while (gtk_events_pending())
			gtk_main_iteration();

		gdk_threads_leave();

		if (quit)
			break;

		iowatch_poll();
	}

	if (playlistpath)
		save_playlist_m3u(playlistpath);

	japlay_exit();

	return 0;
}
