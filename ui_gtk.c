/*
 * japlay GTK user interface
 * Copyright Janne Kulmala 2010
 */
#include "ui.h"
#include "common.h"
#include "japlay.h"
#include "playlist.h"
#include "utils.h"
#include "unixsocket.h"
#include "settings.h"

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <errno.h>

enum {
	COL_ENTRY,
	COL_NAME,
	COL_LENGTH,
	COL_COLOR,
	NUM_COLS
};

struct entry_ui_ctx {
	GtkTreeRowReference *rowref;
};

struct playlist_ui_ctx {
	GtkListStore *store;
};

struct playlist_page {
	struct playlist *playlist;
	GtkWidget *view;
	GtkWidget *page;
};

#define SCOPE_WIDTH		150

size_t ui_song_ctx_size = sizeof(struct entry_ui_ctx);
size_t ui_playlist_ctx_size = sizeof(struct playlist_ui_ctx);

static GtkWidget *main_window;
static GtkWidget *scope_area;
static GtkWidget *seekbar;
static GtkWidget *notebook;
static GThread *main_thread;
static struct playlist_page *pages[64] = {NULL,};
static struct playlist *main_playlist;
static GdkPoint scope_points[SCOPE_WIDTH];

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

/* UI lock must be held */
static void set_playlist_color(struct playlist *playlist,
			       struct playlist_entry *entry, const char *color)
{
	struct playlist_ui_ctx *playlist_ctx = get_playlist_ui_ctx(playlist);
	struct entry_ui_ctx *ctx = get_entry_ui_ctx(entry);
	if (ctx->rowref == NULL)
		return;

	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_ctx->store), &iter, path);
	gtk_tree_path_free(path);

	gtk_list_store_set(playlist_ctx->store, &iter, COL_COLOR, color, -1);
}

static char *get_display_name(struct song *song)
{
	char *title = get_song_title(song);
	if (title)
		return title;
	const char *filename = get_song_filename(song);
	if (memcmp(filename, "http://", 7) == 0)
		return strdup(filename);
	return g_filename_to_utf8(file_base(filename), -1, NULL, NULL, NULL);
}

void ui_add_entry(struct playlist *playlist, struct playlist_entry *after,
		  struct playlist_entry *entry)
{
	struct playlist_ui_ctx *playlist_ctx = get_playlist_ui_ctx(playlist);
	struct song *song = get_entry_song(entry);
	struct entry_ui_ctx *ctx = get_entry_ui_ctx(entry);

	lock_ui();
	GtkTreeIter sibling;
	if (after) {
		struct entry_ui_ctx *after_ctx = get_entry_ui_ctx(after);
		GtkTreePath *path = gtk_tree_row_reference_get_path(after_ctx->rowref);
		gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_ctx->store), &sibling, path);
		gtk_tree_path_free(path);
	}

	GtkTreeIter iter;
	if (after)
		gtk_list_store_insert_after(playlist_ctx->store, &iter, &sibling);
	else
		gtk_list_store_insert_after(playlist_ctx->store, &iter, NULL);
	char *name = get_display_name(song);
	char buf[32];
	unsigned int length = get_song_length(song);
	if (length == -1)
		strcpy(buf, "-");
	else
		sprintf(buf, "%d:%02d", length / (1000 * 60), (length / 1000) % 60);
	gtk_list_store_set(playlist_ctx->store, &iter, COL_ENTRY, entry,
		COL_NAME, name, COL_LENGTH, buf, COL_COLOR, NULL, -1);
	free(name);

	/* store row reference */
	GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(playlist_ctx->store), &iter);
	ctx->rowref = gtk_tree_row_reference_new(GTK_TREE_MODEL(playlist_ctx->store), path);
	gtk_tree_path_free(path);
	unlock_ui();
}

void ui_remove_entry(struct playlist *playlist, struct playlist_entry *entry)
{
	struct playlist_ui_ctx *playlist_ctx = get_playlist_ui_ctx(playlist);
	struct entry_ui_ctx *ctx = get_entry_ui_ctx(entry);

	lock_ui();
	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);
	gtk_tree_row_reference_free(ctx->rowref);

	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_ctx->store), &iter, path);
	gtk_tree_path_free(path);

	gtk_list_store_remove(playlist_ctx->store, &iter);
	unlock_ui();

	ctx->rowref = NULL;
}

static void playlist_clicked(GtkTreeView *view, GtkTreePath *path,
	GtkTreeViewColumn *col, gpointer ptr);

static int add_playlist_page(struct playlist *playlist, const char *title)
{
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(playlist);

	struct playlist_page *page = NEW(struct playlist_page);
	if (page == NULL)
		return -1;
	page->playlist = playlist;

	page->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->store));
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
		GTK_SELECTION_MULTIPLE);
	g_signal_connect(G_OBJECT(page->view), "row-activated", G_CALLBACK(playlist_clicked), NULL);

	/* song name column */
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer), "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
		"Name", renderer, "text", COL_NAME,
		"foreground", COL_COLOR, NULL);
	gtk_tree_view_column_set_expand(column, true);
	gtk_tree_view_insert_column(GTK_TREE_VIEW(page->view), column, -1);

	/* song length column */
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(page->view),
		-1, "Length", renderer, "text", COL_LENGTH,
		"foreground", COL_COLOR, NULL);

	page->page = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page->page),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(page->page), page->view);

	gtk_widget_show_all(page->page);

	int idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page->page,
					   gtk_label_new(title));
	pages[idx] = page;
	return idx;
}

void ui_init_playlist(struct playlist *playlist)
{
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(playlist);

	ctx->store = gtk_list_store_new(NUM_COLS, G_TYPE_POINTER,
		G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

void ui_update_entry(struct playlist *playlist, struct playlist_entry *entry)
{
	struct playlist_ui_ctx *playlist_ctx = get_playlist_ui_ctx(playlist);
	struct entry_ui_ctx *ctx = get_entry_ui_ctx(entry);
	if (ctx->rowref == NULL)
		return;

	lock_ui();
	GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->rowref);
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist_ctx->store), &iter, path);
	gtk_tree_path_free(path);

	char *name = get_display_name(get_entry_song(entry));
	char buf[32];
	unsigned int length = get_song_length(get_entry_song(entry));
	if (length == -1)
		strcpy(buf, "-");
	else
		sprintf(buf, "%d:%02d", length / (1000 * 60), (length / 1000) % 60);
	gtk_list_store_set(playlist_ctx->store, &iter,
		COL_NAME, name, COL_LENGTH, buf, -1);
	free(name);
	unlock_ui();
}

void ui_set_cursor(struct playlist_entry *entry)
{
	GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(seekbar));
	lock_ui();
	if (entry) {
		struct song *song = get_entry_song(entry);
		char *name = get_display_name(song);
		char *buf = concat_strings(name, " - " APP_NAME);
		if (buf) {
			gtk_window_set_title(GTK_WINDOW(main_window), buf);
			free(buf);
		}
		free(name);
		gtk_adjustment_set_upper(adj, get_song_length(song) / 1000.0);
	} else {
		gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
		gtk_adjustment_set_upper(adj, 1);
	}
	unlock_ui();
}

void ui_set_status(int *scope, size_t scope_len, unsigned int position)
{
	if (scope_len > SCOPE_SIZE)
		scope_len = SCOPE_SIZE;

	lock_ui();
	/*gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(power_bar), power / 256.0);
	char buf[10];
	sprintf(buf, "%d:%02d", position / (1000 * 60), (position / 1000) % 60);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(power_bar), buf);*/

	int w = scope_area->allocation.width;
	int h = scope_area->allocation.height;
	int i;
	for (i = 0; i < SCOPE_WIDTH; ++i) {
		scope_points[i].x = i;
		scope_points[i].y = scope[i * scope_len / SCOPE_WIDTH] * h / 2 / SHRT_MAX + h / 2;
	}
	gtk_widget_queue_draw_area(scope_area, 0, 0, w, h);

	GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(seekbar));
	gtk_adjustment_set_value(adj, position / 1000.0);
	unlock_ui();
}

void ui_set_streaming_title(const char *title)
{
	lock_ui();
	char *buf = concat_strings(title, " - " APP_NAME);
	if (buf) {
		gtk_window_set_title(GTK_WINDOW(main_window), buf);
		free(buf);
	}
	unlock_ui();
}

void ui_error(const char *fmt, ...)
{
	GtkWidget *dialog;
	char buf[8192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	lock_ui();
	dialog = gtk_message_dialog_new(GTK_WINDOW(main_window),
					GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_CLOSE,
					buf);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	unlock_ui();
}

void ui_show_message(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	lock_ui();
	GtkWidget *dialog = gtk_dialog_new_with_buttons(APP_NAME, GTK_WINDOW(main_window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL);
	g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(gtk_widget_destroy), NULL);
	GtkWidget *label = gtk_label_new(buf);
	gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), label);
	gtk_widget_show_all(dialog);
	unlock_ui();
}

static struct playlist_page *page_playlist(void)
{
	int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
	return pages[idx];
}

static void add_one_file(char *filename, gpointer ptr)
{
	struct playlist_page *page = page_playlist();

	UNUSED(ptr);
	/* first try to load as a playlist */
	if (load_playlist(page->playlist, filename)) {
		struct playlist_entry *entry =
			add_file_playlist(page->playlist, filename);
		if (entry)
			put_entry(entry);
	}
	g_free(filename);
}

static void add_cb(GtkButton *button, gpointer ptr)
{
	GtkWidget *dialog;

	UNUSED(button);
	UNUSED(ptr);

	struct playlist_page *page = page_playlist();
	if (page->playlist == japlay_queue ||
	    page->playlist == japlay_history) {
		ui_error("Can not add files to this playlist.\n\nPlease select a main or a custom playlist.\n");
		return;
	}

	dialog = gtk_file_chooser_dialog_new("Add Files",
		GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_OPEN,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), true);
	const char *path = get_setting("file_chooser_path");
	if (path) {
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
						    path);
	}

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dialog));
		if (path) {
			set_setting("file_chooser_path", path);
			free(path);
		}
		GSList *filelist = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
		g_slist_foreach(filelist, (GFunc)add_one_file, NULL);
		g_slist_free(filelist);
	}
	gtk_widget_destroy(dialog);
}

static void add_dir_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	GtkWidget *dialog;

	UNUSED(menuitem);
	UNUSED(ptr);

	struct playlist_page *page = page_playlist();
	if (page->playlist == japlay_queue ||
	    page->playlist == japlay_history) {
		ui_error("Can not add files to this playlist.\n\nPlease select a main or a custom playlist.\n");
		return;
	}

	dialog = gtk_file_chooser_dialog_new("Add directory",
		GTK_WINDOW(main_window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);
	const char *path = get_setting("file_chooser_path");
	if (path) {
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
						    path);
	}

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dialog));
		if (path) {
			set_setting("file_chooser_path", path);
			free(path);
		}
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		add_dir_or_file_playlist(page->playlist, filename);
		g_free(filename);
	}
	gtk_widget_destroy(dialog);
}

static void new_playlist_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	UNUSED(menuitem);
	UNUSED(ptr);
	struct playlist *playlist = new_playlist();
	add_playlist_page(playlist, "Extra playlist");
}

static void clear_playlist_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	UNUSED(menuitem);
	UNUSED(ptr);
	clear_playlist(page_playlist()->playlist);
}

static void enable_shuffle_cb(GtkCheckMenuItem *menuitem, gpointer ptr)
{
	UNUSED(ptr);

	bool enabled = gtk_check_menu_item_get_active(menuitem);
	info("shuffle: %d\n", enabled);
	set_playlist_shuffle(japlay_queue, enabled);
}

static void enable_autovol_cb(GtkCheckMenuItem *menuitem, gpointer ptr)
{
	UNUSED(ptr);

	bool enabled = gtk_check_menu_item_get_active(menuitem);
	info("autovol: %d\n", enabled);
	japlay_set_autovol(enabled);
}

static void scan_playlist_cb(GtkMenuItem *menuitem, gpointer ptr)
{
	UNUSED(menuitem);
	UNUSED(ptr);
	start_playlist_scan();
}

static void append_rr_list(GtkTreePath *path, GList **rowref_list)
{
	struct playlist_page *page = page_playlist();
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(page->playlist);

	GtkTreeRowReference *rowref = gtk_tree_row_reference_new(
		GTK_TREE_MODEL(ctx->store), path);
	*rowref_list = g_list_prepend(*rowref_list, rowref);
	gtk_tree_path_free(path);
}

static struct playlist_entry *entry_from_store(GtkListStore *store, GtkTreePath *path)
{
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path);

	GValue value;
	memset(&value, 0, sizeof(value));
	gtk_tree_model_get_value(GTK_TREE_MODEL(store), &iter,
				 COL_ENTRY, &value);
	struct playlist_entry *entry = g_value_get_pointer(&value);
	g_value_unset(&value);

	return entry;
}

static void enqueue_one_file(GtkTreeModel *model, GtkTreePath *path,
			     GtkTreeIter *iter, gpointer ptr)
{
	struct playlist_page *page = page_playlist();
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(page->playlist);

	UNUSED(model);
	UNUSED(iter);
	UNUSED(ptr);
	struct playlist_entry *entry = entry_from_store(ctx->store, path);
	entry = add_playlist(japlay_queue, get_entry_song(entry), false);
	if (entry)
		put_entry(entry);
}

static void enqueue_cb(GtkButton *button, gpointer ptr)
{
	struct playlist_page *page = page_playlist();

	UNUSED(button);
	UNUSED(ptr);
	gtk_tree_selection_selected_foreach(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
		enqueue_one_file, NULL);
}

static void remove_one_file(GtkTreeRowReference *rowref, gpointer ptr)
{
	struct playlist_page *page = page_playlist();
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(page->playlist);

	UNUSED(ptr);
	GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
	gtk_tree_row_reference_free(rowref);

	struct playlist_entry *entry = entry_from_store(ctx->store, path);
	gtk_tree_path_free(path);

	remove_playlist(page->playlist, entry);
}

static void remove_cb(GtkButton *button, gpointer ptr)
{
	struct playlist_page *page = page_playlist();

	UNUSED(button);
	UNUSED(ptr);
	GList *selected = gtk_tree_selection_get_selected_rows(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)), NULL);
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
	struct playlist_page *page = page_playlist();
	if (page->playlist == japlay_queue)
		return;
	struct playlist_ui_ctx *ctx = get_playlist_ui_ctx(page->playlist);

	UNUSED(view);
	UNUSED(col);
	UNUSED(ptr);
	struct playlist_entry *entry = entry_from_store(ctx->store, path);
	entry = add_playlist(japlay_queue, get_entry_song(entry), true);
	if (entry) {
		/* possible race with the playback engine */
		japlay_skip();
		put_entry(entry);
	}
}

static void destroy_cb(GtkWidget *widget, gpointer ptr)
{
	UNUSED(widget);
	UNUSED(ptr);
	gtk_main_quit();
}

static gboolean seek_cb(GtkRange *range, GtkScrollType scroll, double value,
			gpointer ptr)
{
	UNUSED(range);
	UNUSED(scroll);
	UNUSED(ptr);
	japlay_seek(value * 1000.0);
	return true;
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

static char *format_seek_cb(GtkScale *scale, gdouble value)
{
	UNUSED(scale);
	unsigned int secs = value;
	return g_strdup_printf("%d:%02d", secs / 60, secs % 60);
}

static void handle_sigint(int sig)
{
	UNUSED(sig);
	gtk_main_quit();
}

static gboolean incoming_data(GIOChannel *io, GIOCondition cond, gpointer ptr)
{
	int fd = g_io_channel_unix_get_fd(io);
	UNUSED(cond);
	UNUSED(ptr);

	char filename[FILENAME_MAX + 1];
	ssize_t len = recvfrom(fd, filename, FILENAME_MAX, 0, NULL, 0);
	if (len < 0) {
		if (errno == EAGAIN)
			return 0;
		warning("recv failed (%s)\n", strerror(errno));
		g_io_channel_close(io);
		return FALSE;
	}
	if (len == 0) {
		g_io_channel_close(io);
		return FALSE;
	}
	filename[len] = 0;
	struct playlist_entry *entry = add_file_playlist(main_playlist, filename);
	if (entry)
		put_entry(entry);
	return TRUE;
}

static gboolean incoming_client(GIOChannel *io, GIOCondition cond, gpointer ptr)
{
	int fd = g_io_channel_unix_get_fd(io);
	UNUSED(cond);
	UNUSED(ptr);

	struct sockaddr_un addr;
	socklen_t addrlen = sizeof(addr);
	int rfd = accept(fd, (struct sockaddr *)&addr, &addrlen);
	if (rfd < 0) {
		warning("accept failure (%s)\n", strerror(errno));
		return TRUE;
	}
	io = g_io_channel_unix_new(rfd);
	g_io_add_watch(io, G_IO_IN, incoming_data, NULL);
	return TRUE;
}

gboolean expose_event_cb(GtkWidget *widget, GdkEventExpose *event, gpointer ptr)
{
	UNUSED(ptr);
	UNUSED(event);

	int w = widget->allocation.width;
	int h = widget->allocation.height;
	gdk_draw_rectangle(widget->window, widget->style->black_gc,
			   true, 0, 0, w, h);
	gdk_draw_lines(widget->window, widget->style->white_gc,
		       scope_points, SCOPE_WIDTH);
	return true;
}

int main(int argc, char **argv)
{
	char *socketname = get_config_name("control");

	/* check if japlay is already running */
	if (socketname && file_exists(socketname)) {
		int fd = unix_socket_connect(socketname);
		if (fd >= 0) {
			int i;
			for (i = 1; i < argc; ++i) {
				char *path = absolute_path(argv[i]);
				if (path) {
					sendto(fd, path, strlen(path), 0, NULL, 0);
					free(path);
				}
			}
			close(fd);
			return 0;
		}
		/* remove leftover socket */
		unlink(socketname);
	}

	g_thread_init(NULL);
	gdk_threads_init();
	gdk_threads_enter();
	gtk_init(&argc, &argv);

	main_thread = g_thread_self();

	if (japlay_init(&argc, argv)) {
		error("Can not initialize japlay\n");
		return -1;
	}

	main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
	g_signal_connect(G_OBJECT(main_window), "destroy", G_CALLBACK(destroy_cb), NULL);
	g_signal_connect(G_OBJECT(main_window), "key-press-event", G_CALLBACK(key_pressed_cb), NULL);

	GtkWidget *menubar = gtk_menu_bar_new();

	GtkWidget *file_menu = gtk_menu_new();

	GtkWidget *item = gtk_menu_item_new_with_label("New playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(new_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Add directory to the playlist...");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(add_dir_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Clear playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(clear_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_separator_menu_item_new();
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_check_menu_item_new_with_label("Enable shuffle");
	g_signal_connect(G_OBJECT(item), "toggled", G_CALLBACK(enable_shuffle_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_check_menu_item_new_with_label("Enable automatic volume");
	g_signal_connect(G_OBJECT(item), "toggled", G_CALLBACK(enable_autovol_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	/*item = gtk_menu_item_new_with_label("Scan playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(scan_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);*/

	item = gtk_separator_menu_item_new();
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Quit");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(destroy_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("File");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), file_menu);
	gtk_menu_bar_append(GTK_MENU_BAR(menubar), item);

	static const struct button {
		const char *stockid;
		const char *help;
		void (*const cb)(GtkButton *button, gpointer ptr);
	} buttons[] = {
		{GTK_STOCK_MEDIA_PLAY, "Play", play_cb},
		{GTK_STOCK_MEDIA_STOP, "stop", stop_cb},
		{GTK_STOCK_MEDIA_PAUSE, "Pause", pause_cb},
		{GTK_STOCK_MEDIA_NEXT, "Skip to the next song in the queue", next_cb},
		{GTK_STOCK_OPEN, "Add files to the playlist", add_cb},
		{GTK_STOCK_OK, "Add selected songs to the queue", enqueue_cb},
		{GTK_STOCK_DELETE, "Remove selected songs", remove_cb},
		{NULL, NULL, NULL}
	};

	GtkWidget *toolbar = gtk_hbox_new(false, 0);
	int i;
	for (i = 0; buttons[i].stockid; ++i) {
		GtkWidget *button = gtk_button_new();
		GtkWidget *image = gtk_image_new_from_stock(buttons[i].stockid, GTK_ICON_SIZE_SMALL_TOOLBAR);
		gtk_button_set_image(GTK_BUTTON(button), image);
		g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(buttons[i].cb), NULL);
		gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);
		gtk_widget_set_tooltip_text(button, buttons[i].help);
	}

	scope_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(scope_area, SCOPE_WIDTH, -1);
	gtk_box_pack_start(GTK_BOX(toolbar), scope_area, false, true, 0);
	g_signal_connect(G_OBJECT(scope_area), "expose_event", G_CALLBACK(expose_event_cb), NULL);

	seekbar = gtk_hscale_new_with_range(0, 1, 1);
	gtk_range_set_update_policy(GTK_RANGE(seekbar), GTK_UPDATE_DELAYED);
	g_signal_connect(G_OBJECT(seekbar), "change-value", G_CALLBACK(seek_cb), NULL);
	g_signal_connect(G_OBJECT(seekbar), "format-value", G_CALLBACK(format_seek_cb), NULL);

	notebook = gtk_notebook_new();

	GtkWidget *vbox = gtk_vbox_new(false, 0);
	gtk_box_pack_start(GTK_BOX(vbox), menubar, false, true, 0);
	gtk_box_pack_start(GTK_BOX(vbox), toolbar, false, true, 0);
	gtk_box_pack_start(GTK_BOX(vbox), seekbar, false, true, 0);
	gtk_box_pack_start(GTK_BOX(vbox), notebook, true, true, 0);

	gtk_container_add(GTK_CONTAINER(main_window), vbox);
	gtk_widget_show_all(main_window);

	/* TODO: load all playlists */
	main_playlist = new_playlist();

	add_playlist_page(main_playlist, "Main");
	add_playlist_page(japlay_queue, "Play queue");
	add_playlist_page(japlay_history, "History");

	char *playlistpath = get_config_name("main_playlist.m3u");
	if (playlistpath)
		load_playlist(main_playlist, playlistpath);
	char *queuepath = get_config_name("queue.m3u");
	if (queuepath)
		load_playlist(japlay_queue, queuepath);

	char *settingspath = get_config_name("settings.cfg");
	if (settingspath)
		load_settings(settingspath);

	for (i = 1; i < argc; ++i)
		add_file_playlist(main_playlist, argv[i]);

	signal(SIGINT, handle_sigint);

	int fd = -1;
	if (socketname) {
		fd = unix_socket_create(socketname);
		if (fd >= 0) {
			GIOChannel *io = g_io_channel_unix_new(fd);
			g_io_add_watch(io, G_IO_IN, incoming_client, NULL);
		}
	}

	gtk_main();

	if (fd >= 0)
		unlink(socketname);

	if (playlistpath)
		save_playlist_m3u(main_playlist, playlistpath);
	if (queuepath)
		save_playlist_m3u(japlay_queue, queuepath);
	if (settingspath)
		save_settings(settingspath);

	japlay_exit();

	return 0;
}
