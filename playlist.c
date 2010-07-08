#include "playlist.h"
#include "common.h"
#include "ui.h"
#include "list.h"
#include <glib.h>
#include <stdlib.h>
#include <pthread.h>

static struct list_head playlist;
static unsigned int playlist_len = 0;

static pthread_spinlock_t refcountspinlock;
static pthread_mutex_t playlist_mutex;

#define REF_COUNT_LOCK pthread_spin_lock(&refcountspinlock)
#define REF_COUNT_UNLOCK pthread_spin_unlock(&refcountspinlock)

#define PLAYLIST_LOCK pthread_mutex_lock(&playlist_mutex)
#define PLAYLIST_UNLOCK pthread_mutex_unlock(&playlist_mutex)

struct song {
	struct list_head head;
	bool reliable_length;
	unsigned int refcount, length;
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

unsigned int get_song_length(struct song *song)
{
	return song->length;
}

struct song *new_song(const char *filename)
{
	struct song *song = NEW(struct song);
	if (!song)
		return NULL;
	song->filename = strdup(filename);
	song->refcount = 1;
	song->length = (unsigned int) -1;
	song->ui_ctx = malloc(ui_song_ctx_size);

	return song;
}

void get_song(struct song *song)
{
	REF_COUNT_LOCK;
	song->refcount++;
	REF_COUNT_UNLOCK;
}

void put_song(struct song *song)
{
	REF_COUNT_LOCK;
	song->refcount--;
	if (!song->refcount) {
		free(song->filename);
		free(song->ui_ctx);
		free(song);
	}
	REF_COUNT_UNLOCK;
}

void set_song_length(struct song *song, unsigned int length, bool reliable)
{
	if (reliable || !song->reliable_length) {
		song->length = length;
		song->reliable_length = reliable;
		ui_update_playlist(song);
	}
}

struct song *playlist_next(struct song *song, bool forward)
{
	struct song *next = NULL;
	PLAYLIST_LOCK;
	if (song->head.next) {
		struct list_head *pos;
		if (forward)
			pos = song->head.next;
		else
			pos = song->head.prev;
		if (pos != &playlist) {
			next = container_of(pos, struct song, head);
			get_song(next);
		}
	}
	PLAYLIST_UNLOCK;
	return next;
}

struct song *get_playlist_first(void)
{
	PLAYLIST_LOCK;
	struct song *song = NULL;
	if (!list_empty(&playlist)) {
		song = container_of(playlist.next, struct song, head);
		get_song(song);
	}
	PLAYLIST_UNLOCK;

	return song;
}

void add_playlist(struct song *song)
{
	get_song(song);

	PLAYLIST_LOCK;
	list_add_tail(&song->head, &playlist);
	ui_add_playlist(song);
	playlist_len++;
	PLAYLIST_UNLOCK;
}

void remove_playlist(struct song *song)
{
	PLAYLIST_LOCK;
	list_del(&song->head);
	playlist_len--;
	memset(&song->head, 0, sizeof(song->head));
	ui_remove_playlist(song);
	PLAYLIST_UNLOCK;

	put_song(song);
}

void clear_playlist(void)
{
	struct list_head *pos, *next;

	PLAYLIST_LOCK;
	list_for_each_safe(pos, next, &playlist) {
		struct song *song = container_of(pos, struct song, head);
		memset(&song->head, 0, sizeof(song->head));
		ui_remove_playlist(song);
		put_song(song);
	}
	list_init(&playlist);
	playlist_len = 0;
	PLAYLIST_UNLOCK;
}

void shuffle_playlist(void)
{
	struct list_head *pos;

	PLAYLIST_LOCK;
	struct song **table = malloc(sizeof(void *) * playlist_len);
	unsigned int len = 0;
	list_for_each(pos, &playlist) {
		struct song *song = container_of(pos, struct song, head);
		unsigned int i = rand() % (len + 1);
		if (i != len)
			memmove(&table[i + 1], &table[i], (len - i) * sizeof(table[0]));
		table[i] = song;
		ui_remove_playlist(song);
		len++;
	}
	list_init(&playlist);
	unsigned int i;
	for (i = 0; i < playlist_len; ++i) {
		struct song *song = table[i];
		list_add_tail(&song->head, &playlist);
		ui_add_playlist(song);
	}
	free(table);
	PLAYLIST_UNLOCK;
}

bool save_playlist_m3u(const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	struct list_head *pos;

	PLAYLIST_LOCK;
	list_for_each(pos, &playlist) {
		struct song *song = container_of(pos, struct song, head);
		fprintf(f, "%s\n", song->filename);
		if (song->length != -1)
			fprintf(f, "#length %d\n", song->length);
	}
	PLAYLIST_UNLOCK;

	fclose(f);
	return true;
}

void init_playlist(void)
{
	pthread_spin_init(&refcountspinlock, 0);
	pthread_mutex_init(&playlist_mutex, NULL);
	list_init(&playlist);
}
