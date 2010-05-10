#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <ao/ao.h>
#include <mad.h>

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)x

enum {
	COL_FILENAME,
	COL_NAME,
	COL_COLOR,
	NUM_COLS
};

static GtkWidget *main_window;
static GtkWidget *playlist_view;
static GtkListStore *playlist;
static gchar *playfilename = NULL;
static gchar *changefilename = NULL;
static GtkTreeRowReference *playing_rowref = NULL;
static GMutex *play_mutex;
static GCond *play_cond;
static bool stop = true, reset = false;

static gchar *set_playing_path(GtkTreePath *path)
{
	GtkTreeIter iter;
	if (playing_rowref) {
		GtkTreePath *path = gtk_tree_row_reference_get_path(playing_rowref);
		if (path) {
			gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist), &iter, path);
			gtk_tree_path_free(path);
			gtk_list_store_set(playlist, &iter, COL_COLOR, NULL, -1);
		}
		gtk_tree_row_reference_free(playing_rowref);
		playing_rowref = NULL;
	}
	if (!path) {
		gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
		return NULL;
	}
	playing_rowref = gtk_tree_row_reference_new(GTK_TREE_MODEL(playlist), path);

	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist), &iter, path);
	gtk_list_store_set(playlist, &iter, COL_COLOR, "red", -1);
	GValue value;
	memset(&value, 0, sizeof(value));
	gtk_tree_model_get_value(GTK_TREE_MODEL(playlist), &iter, COL_FILENAME, &value);
	gchar *filename = g_value_dup_string(&value);
	g_value_unset(&value);
	gtk_tree_model_get_value(GTK_TREE_MODEL(playlist), &iter, COL_NAME, &value);

	gchar *buf = g_malloc(strlen(filename) + 32);
	strcpy(buf, g_value_get_string(&value));
	strcat(buf, " - " APP_NAME);
	gtk_window_set_title(GTK_WINDOW(main_window), buf);
	g_free(buf);
	g_value_unset(&value);

	return filename;
}

static gchar *advance_playlist()
{
	GtkTreeIter iter;
	if (!playing_rowref)
		return NULL;
	GtkTreePath *path = gtk_tree_row_reference_get_path(playing_rowref);
	if (!path)
		return NULL;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist), &iter, path);
	gtk_tree_path_free(path);
	if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist), &iter))
		return NULL;
	path = gtk_tree_model_get_path(GTK_TREE_MODEL(playlist), &iter);
	gchar *filename = set_playing_path(path);
	gtk_tree_path_free(path);

	return filename;
}

static int scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static gpointer play_thread(gpointer ptr)
{
	UNUSED(ptr);
	static struct mad_stream stream;
	static struct mad_frame frame;
	static struct mad_synth synth;

	mad_frame_init(&frame);
	mad_stream_init(&stream);
	mad_synth_init(&synth);

	static unsigned char buffer[8192];
	static int16_t output[8192];

	ao_device *dev = NULL;
	int fd = -1;
	ao_sample_format format = {.bits = 16, .byte_format = AO_FMT_NATIVE,};

	while (true) {
		g_mutex_lock(play_mutex);
		if (stop) {
			if (reset) {
				if (fd > 0)
					close(fd);
				if (playfilename)
					g_free(playfilename);
				mad_stream_init(&stream);
				playfilename = NULL;
				fd = -1;
			}
			g_cond_wait(play_cond, play_mutex);
			reset = false;
			stop = false;
		}

		if (changefilename) {
			if (playfilename)
				g_free(playfilename);
			playfilename = changefilename;
			changefilename = NULL;
			if (fd > 0)
				close(fd);
			fd = open(playfilename, O_RDONLY);
		}
		g_mutex_unlock(play_mutex);

		if (!playfilename) {
			stop = true;
			continue;
		}

		size_t len = 0;
		if (stream.next_frame) {
			len = stream.bufend - stream.next_frame;
			if (len)
				memcpy(buffer, stream.next_frame, len);
		}
		len += read(fd, &buffer[len], sizeof(buffer) - len);
		if (len < sizeof(buffer)) {
			gdk_threads_enter();
			changefilename = advance_playlist();
			gdk_threads_leave();
		}

		mad_stream_buffer(&stream, buffer, len);

		if (mad_header_decode(&frame.header, &stream)) {
			g_printf("MAD error: %s\n", mad_stream_errorstr(&stream));
			continue;
		}

		if (frame.header.samplerate != (unsigned int) format.rate ||
		    MAD_NCHANNELS(&frame.header) != format.channels || !dev) {
			if (dev)
				ao_close(dev);
			g_printf("format change: %d Hz, %d channels\n",
				frame.header.samplerate, MAD_NCHANNELS(&frame.header));
			format.rate = frame.header.samplerate;
			format.channels = MAD_NCHANNELS(&frame.header);
			dev = ao_open_live(ao_default_driver_id(), &format, NULL);
			if (!dev) {
				g_printf("Unable to open audio device\n");
				stop = true;
			}
		}

		if (mad_frame_decode(&frame, &stream)) {
			g_printf("MAD error: %s\n", mad_stream_errorstr(&stream));
			continue;
		}

		mad_synth_frame(&synth, &frame);

		if (MAD_NCHANNELS(&frame.header) == 2) {
			size_t i;
			for (i = 0; i < synth.pcm.length; ++i) {
				output[i * 2] = scale(synth.pcm.samples[0][i]);
				output[i * 2 + 1] = scale(synth.pcm.samples[1][i]);
			}
		} else if (MAD_NCHANNELS(&frame.header) == 1) {
			size_t i;
			for (i = 0; i < synth.pcm.length; ++i)
				output[i] = scale(synth.pcm.samples[0][i]);
		}
		ao_play(dev, (char *)output, synth.pcm.length * 2 *
			MAD_NCHANNELS(&frame.header));
	}

	return NULL;
}

static const gchar *file_ext(const gchar *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/') {
		if (filename[i - 1] == '.')
			return &filename[i];
		--i;
	}
	return NULL;
}

static const gchar *file_base(const gchar *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/')
		--i;
	return &filename[i];
}

static const gchar *build_filename(gchar *buf, const gchar *orig, const gchar *base)
{
	if (!memcmp(base, "http:", 5) || base[0] == '/')
		return base;
	size_t i = strlen(orig);
	while (i && orig[i - 1] != '/')
		--i;
	memcpy(buf, orig, i);
	strcpy(&buf[i], base);
	return buf;
}

static void add_playlist(const gchar *filename)
{
	if (!filename[0])
		return;
	GtkTreeIter iter;
	const gchar *name;
	g_printf("adding %s\n", filename);
	if (!memcmp(filename, "http:", 5))
		name = filename;
	else
		name = file_base(filename);
	gtk_list_store_append(playlist, &iter);
	gtk_list_store_set(playlist, &iter, COL_FILENAME, filename,
		COL_NAME, name, COL_COLOR, NULL, -1);
}

static gchar *trim(gchar *buf)
{
	size_t i = strlen(buf);
	while (i && isspace(buf[i - 1]))
		--i;
	buf[i] = 0;
	i = 0;
	while (isspace(buf[i]))
		++i;
	return &buf[i];
}

static bool load_playlist_pls(const gchar *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return false;

	char row[256];
	while (fgets(row, sizeof(row), f)) {
		size_t i;
		gchar *value = NULL;
		for (i = 0; row[i]; ++i) {
			if (row[i] == '=') {
				row[i] = 0;
				value = &row[i + 1];
				break;
			}
		}
		if (!memcmp(trim(row), "File", 4) && value) {
			gchar buf[256];
			add_playlist(build_filename(buf, filename, trim(value)));
		}
	}
	fclose(f);
	return true;
}

static bool load_playlist_m3u(const gchar *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return false;

	gchar row[256];
	while (fgets(row, sizeof(row), f)) {
		if (row[0] != '#') {
			gchar buf[256];
			add_playlist(build_filename(buf, filename, trim(row)));
		}
	}
	fclose(f);
	return true;
}

static bool save_playlist_m3u(const gchar *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	GtkTreeIter iter;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist), &iter)) {
		while (true) {
			GValue value;
			memset(&value, 0, sizeof(value));
			gtk_tree_model_get_value(GTK_TREE_MODEL(playlist), &iter, COL_FILENAME, &value);
			fputs(g_value_get_string(&value), f);
			fputc('\n', f);
			g_value_unset(&value);
			if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(playlist), &iter))
				break;
		}
	}
	fclose(f);
	return true;
}

static void add_one_file(gchar *filename, gpointer ptr)
{
	UNUSED(ptr);
	const gchar *ext = file_ext(filename);
	if (ext) {
		if (!strcasecmp(ext, "pls"))
			load_playlist_pls(filename);
		else if (!strcasecmp(ext, "m3u"))
			load_playlist_m3u(filename);
		else
			add_playlist(filename);
	} else
		add_playlist(filename);
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
	gtk_list_store_clear(playlist);
}

static void append_rr_list(GtkTreePath *path, GList **rowref_list)
{
	GtkTreeRowReference *rowref = gtk_tree_row_reference_new(
		GTK_TREE_MODEL(playlist), path);
	*rowref_list = g_list_append(*rowref_list, rowref);
	gtk_tree_path_free(path);
}

static void remove_one_file(GtkTreeRowReference *rowref, gpointer ptr)
{
	UNUSED(ptr);
	GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(playlist), &iter, path);
	gtk_tree_path_free(path);
	gtk_tree_row_reference_free(rowref);
	gtk_list_store_remove(playlist, &iter);
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
	if (!playfilename) {
		GtkTreeIter iter;
		if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlist), &iter))
			return;
		GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(playlist), &iter);
		changefilename = set_playing_path(path);
		gtk_tree_path_free(path);
	}
	g_cond_signal(play_cond);
}

static void pause_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	stop = true;
}

static void next_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	changefilename = advance_playlist();
	g_cond_signal(play_cond);
}

static void shuffle_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
}

static void stop_cb(GtkButton *button, gpointer ptr)
{
	UNUSED(button);
	UNUSED(ptr);
	g_mutex_lock(play_mutex);
	reset = true;
	stop = true;
	g_mutex_unlock(play_mutex);
}

static void playlist_clicked(GtkTreeView *view, GtkTreePath *path,
	GtkTreeViewColumn *col, gpointer ptr)
{
	UNUSED(view);
	UNUSED(col);
	UNUSED(ptr);
	changefilename = set_playing_path(path);
	g_cond_signal(play_cond);
}

static void destroy_cb(GtkWidget *widget, gpointer ptr)
{
	UNUSED(widget);
	UNUSED(ptr);
	gtk_main_quit();
}

int main(int argc, char **argv)
{
	g_thread_init(NULL);
	gdk_threads_init();

	ao_initialize();

	play_mutex = g_mutex_new();
	play_cond = g_cond_new();
	g_thread_create(play_thread, NULL, false, NULL);

	gdk_threads_enter();
	gtk_init(&argc, &argv);

	main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(main_window), APP_NAME);
	g_signal_connect(G_OBJECT(main_window), "destroy", G_CALLBACK(destroy_cb), NULL);

	playlist = gtk_list_store_new(NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

	GtkWidget *menubar = gtk_menu_bar_new();

	GtkWidget *file_menu = gtk_menu_new();

	GtkWidget *item = gtk_menu_item_new_with_label("Clear playlist");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(clear_playlist_cb), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("Quit");
	g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(gtk_main_quit), NULL);
	gtk_menu_append(GTK_MENU(file_menu), item);

	item = gtk_menu_item_new_with_label("File");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), file_menu);
	gtk_menu_bar_append(GTK_MENU_BAR(menubar), item);

	GtkWidget *toolbar = gtk_hbox_new(false, 0);
	GtkWidget *button = gtk_button_new_from_stock(GTK_STOCK_OPEN);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(open_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	button = gtk_button_new_from_stock(GTK_STOCK_MEDIA_PLAY);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(play_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	button = gtk_button_new_from_stock(GTK_STOCK_MEDIA_STOP);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(stop_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	button = gtk_button_new_from_stock(GTK_STOCK_MEDIA_PAUSE);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(pause_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	button = gtk_button_new_from_stock(GTK_STOCK_MEDIA_NEXT);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(next_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	button = gtk_button_new_from_stock(GTK_STOCK_DELETE);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(delete_cb), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), button, false, true, 0);

	playlist_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlist));
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(playlist_view)),
		GTK_SELECTION_MULTIPLE);
	g_signal_connect(G_OBJECT(playlist_view), "row-activated", G_CALLBACK(playlist_clicked), NULL);

	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(playlist_view),
		-1, "Name", renderer, "text", COL_NAME, "foreground", COL_COLOR, NULL);

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

	char buf[256];
	strcpy(buf, getenv("HOME"));
	strcat(buf, "/.japlay/playlist.m3u");
	load_playlist_m3u(buf);

	int i;
	for (i = 1; i < argc; ++i)
		add_playlist(argv[i]);

	gtk_main();

	save_playlist_m3u(buf);

	gdk_threads_leave();

	return 0;
}
