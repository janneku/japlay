#include "playlist.h"
#include "common.h"
#include "ui.h"
#include "atomic.h"
#include "list.h"
#include "utils.h"
#include <glib.h>
#include <stdlib.h>

static GMutex *playlist_mutex;
static struct list_head playlist;
static int playlist_len = 0;

struct song {
	struct list_head head;
	atomic_t refcount;
	char *filename;
	struct song_ui_ctx *ui_ctx;
};

struct song_ui_ctx *get_song_ui_ctx(struct song *song)
{
	return song->ui_ctx;
}

const char *get_song_filename(struct song *song)
{
	return song->filename;
}

struct song *new_song(const char *filename)
{
	char *fname = absolute_path(filename);
	if (!fname)
		return NULL;
	printf("adding %s\n", fname);

	struct song *song = NEW(struct song);
	if (!song) {
		free(fname);
		return NULL;
	}
	song->filename = fname;
	atomic_set(&song->refcount, 1);
	song->ui_ctx = malloc(ui_song_ctx_size);

	return song;
}

void get_song(struct song *song)
{
	atomic_inc(&song->refcount);
}

void put_song(struct song *song)
{
	if (atomic_dec_and_test(&song->refcount)) {
		free(song->filename);
		free(song->ui_ctx);
		free(song);
	}
}

struct song *playlist_next(struct song *song, bool forward)
{
	g_mutex_lock(playlist_mutex);
	if (song->head.next) {
		struct list_head *pos;
		if (forward)
			pos = song->head.next;
		else
			pos = song->head.prev;
		if (pos != &playlist) {
			struct song *song = list_container(pos, struct song, head);
			get_song(song);
			g_mutex_unlock(playlist_mutex);
			return song;
		}
	}
	g_mutex_unlock(playlist_mutex);
	return NULL;
}

struct song *get_playlist_first(void)
{
	g_mutex_lock(playlist_mutex);
	struct song *song = list_container(playlist.next,
				struct song, head);
	get_song(song);
	g_mutex_unlock(playlist_mutex);

	return song;
}

void add_playlist(struct song *song)
{
	get_song(song);

	g_mutex_lock(playlist_mutex);
	list_add_tail(&song->head, &playlist);
	ui_add_playlist(song);
	playlist_len++;
	g_mutex_unlock(playlist_mutex);
}

void remove_playlist(struct song *song)
{
	g_mutex_lock(playlist_mutex);
	list_del(&song->head);
	playlist_len--;
	memset(&song->head, 0, sizeof(song->head));
	ui_remove_playlist(song);
	g_mutex_unlock(playlist_mutex);

	put_song(song);
}

void clear_playlist(void)
{
	struct list_head *pos, *next;

	g_mutex_lock(playlist_mutex);
	list_for_each_safe(pos, next, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		memset(&song->head, 0, sizeof(song->head));
		ui_remove_playlist(song);
		put_song(song);
	}
	list_init(&playlist);
	playlist_len = 0;
	g_mutex_unlock(playlist_mutex);
}

void shuffle_playlist(void)
{
	struct list_head *pos;

	g_mutex_lock(playlist_mutex);
	struct song **table = malloc(sizeof(void *) * playlist_len);
	int len = 0;
	list_for_each(pos, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		int i = rand() % (len + 1);
		if (i != len)
			memmove(&table[i + 1], &table[i], (len - i) * sizeof(table[0]));
		table[i] = song;
		ui_remove_playlist(song);
		len++;
	}
	list_init(&playlist);
	int i;
	for (i = 0; i < playlist_len; ++i) {
		struct song *song = table[i];
		list_add_tail(&song->head, &playlist);
		ui_add_playlist(song);
	}
	free(table);
	g_mutex_unlock(playlist_mutex);
}

bool save_playlist_m3u(const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	struct list_head *pos;

	g_mutex_lock(playlist_mutex);
	list_for_each(pos, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		fputs(song->filename, f);
		fputc('\n', f);
	}
	g_mutex_unlock(playlist_mutex);

	fclose(f);
	return true;
}

void init_playlist(void)
{
	list_init(&playlist);

	playlist_mutex = g_mutex_new();
}
