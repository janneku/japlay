/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#define _POSIX_C_SOURCE 200112L	/* spinlocks */

#include "playlist.h"
#include "common.h"
#include "japlay.h"
#include "ui.h"
#include "list.h"
#include "utils.h"
#include "hashmap.h"
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

static struct hashmap song_map;

static pthread_spinlock_t refcountspinlock;
static pthread_mutex_t database_mutex;

#define REF_COUNT_LOCK pthread_spin_lock(&refcountspinlock)
#define REF_COUNT_UNLOCK pthread_spin_unlock(&refcountspinlock)

#define DATABASE_LOCK pthread_mutex_lock(&database_mutex)
#define DATABASE_UNLOCK pthread_mutex_unlock(&database_mutex)

#define PLAYLIST_LOCK(playlist) pthread_mutex_lock(&(playlist)->mutex)
#define PLAYLIST_UNLOCK(playlist) pthread_mutex_unlock(&(playlist)->mutex)

struct song {
	struct hash_node node;
	int length_score;
	unsigned int refcount, length;
	char *filename, *title;
	struct list_head entries;
};

struct playlist_entry {
	struct playlist *playlist;
	struct list_head head;
	struct list_head song_head;
	unsigned int refcount;
	struct song *song;
	struct entry_ui_ctx *ui_ctx;
};

struct playlist {
	struct list_head entries;
	pthread_mutex_t mutex;
	const char *name;
	unsigned int len;
	struct playlist_ui_ctx *ui_ctx;
	bool shuffle;
};

struct song *get_entry_song(struct playlist_entry *entry)
{
	/* NOTE: this does NOT increase refcount! */
	return entry->song;
}

struct entry_ui_ctx *get_entry_ui_ctx(struct playlist_entry *entry)
{
	return entry->ui_ctx;
}

struct playlist_ui_ctx *get_playlist_ui_ctx(struct playlist *playlist)
{
	return playlist->ui_ctx;
}

const char *get_song_filename(struct song *song)
{
	return song->filename;
}

char *get_song_title(struct song *song)
{
	/* TODO: locking */
	if (song->title)
		return strdup(song->title);
	return NULL;
}

const char *get_playlist_name(struct playlist *playlist)
{
	return playlist->name;
}

unsigned int get_song_length(struct song *song)
{
	return song->length;
}

void set_playlist_shuffle(struct playlist *playlist, bool enabled)
{
	playlist->shuffle = enabled;
}

static struct song *find_song_locked(const char *filename)
{
	struct hash_node *node = hashmap_get(&song_map, (void *)filename);
	if (node == NULL)
		return NULL;
	struct song *song = container_of(node, struct song, node);
	get_song(song);
	return song;
}

struct song *find_song(const char *filename)
{
	DATABASE_LOCK;
	struct song *song = find_song_locked(filename);
	DATABASE_UNLOCK;
	return song;
}

struct playlist *new_playlist(const char *name)
{
	struct playlist *playlist = NEW(struct playlist);
	if (playlist == NULL)
		return NULL;
	pthread_mutex_init(&playlist->mutex, NULL);
	playlist->name = strdup(name);
	playlist->ui_ctx = calloc(1, ui_playlist_ctx_size);
	list_init(&playlist->entries);
	ui_show_playlist(playlist);
	return playlist;
}

struct song *new_song(const char *filename)
{
	DATABASE_LOCK;
	struct song *song = find_song_locked(filename);
	if (song == NULL) {
		song = NEW(struct song);
		if (song) {
			list_init(&song->entries);
			song->filename = strdup(filename);
			song->refcount = 1;
			song->length = (unsigned int) -1;
			hashmap_insert(&song_map, &song->node, (void *)filename);
		}
	}
	DATABASE_UNLOCK;
	return song;
}

void get_song(struct song *song)
{
	REF_COUNT_LOCK;
	assert(song->refcount > 0);
	song->refcount++;
	REF_COUNT_UNLOCK;
}

void put_song(struct song *song)
{
	REF_COUNT_LOCK;
	assert(song->refcount > 0);
	song->refcount--;
	bool zero = (song->refcount == 0);
	REF_COUNT_UNLOCK;

	if (zero) {
		DATABASE_LOCK;
		assert(hashmap_remove(&song_map, (void *)song->filename) != NULL);
		DATABASE_UNLOCK;
		free(song->filename);
		free(song->title);
		free(song);
	}
}

void get_entry(struct playlist_entry *entry)
{
	REF_COUNT_LOCK;
	assert(entry->refcount > 0);
	entry->refcount++;
	REF_COUNT_UNLOCK;
}

void put_entry(struct playlist_entry *entry)
{
	REF_COUNT_LOCK;
	assert(entry->refcount > 0);
	entry->refcount--;
	bool zero = (entry->refcount == 0);
	REF_COUNT_UNLOCK;

	if (zero) {
		assert(entry->playlist == NULL);
		DATABASE_LOCK;
		list_del(&entry->song_head);
		DATABASE_UNLOCK;
		put_song(entry->song);
		free(entry->ui_ctx);
		free(entry);
	}
}

void set_song_length(struct song *song, unsigned int length, int score)
{
	/* Higher the score is, more reliable length of the song is */
	if (score >= song->length_score) {
		song->length = length;
		song->length_score = score;

		/* notify UI */
		struct list_head *pos;
		DATABASE_LOCK;
		list_for_each(pos, &song->entries) {
			struct playlist_entry *entry
				= container_of(pos, struct playlist_entry, song_head);
			ui_update_entry(entry->playlist, entry);
		}
		DATABASE_UNLOCK;
	}
}

void set_song_title(struct song *song, const char *str)
{
	/* TODO: locking */
	free(song->title);
	song->title = strdup(str);

	/* notify UI */
	struct list_head *pos;
	DATABASE_LOCK;
	list_for_each(pos, &song->entries) {
		struct playlist_entry *entry
			= container_of(pos, struct playlist_entry, song_head);
		ui_update_entry(entry->playlist, entry);
	}
	DATABASE_UNLOCK;
}

struct playlist_entry *get_playlist_first(struct playlist *playlist)
{
	PLAYLIST_LOCK(playlist);
	struct playlist_entry *entry = NULL;
	if (!list_empty(&playlist->entries)) {
		entry = container_of(playlist->entries.next, struct playlist_entry, head);
		get_entry(entry);
	}
	PLAYLIST_UNLOCK(playlist);

	return entry;
}

struct playlist_entry *add_playlist(struct playlist *playlist, struct song *song,
				    bool first)
{
	struct playlist_entry *entry = NEW(struct playlist_entry);
	if (entry == NULL)
		return NULL;
	entry->playlist = playlist;
	entry->ui_ctx = calloc(1, ui_song_ctx_size);
	entry->refcount = 2; /* yes, return with refcount of two */
	get_song(song);
	entry->song = song;

	PLAYLIST_LOCK(playlist);
	struct playlist_entry *after = NULL;
	if (first) {
		/* nothing */
	} else if (playlist->shuffle) {
		/* pick a random position */
		int i = rand() % (playlist->len + 1);
		if (i) {
			struct list_head *pos = playlist->entries.next;
			i--;
			while (i) {
				pos = pos->next;
				i--;
			}
			after = container_of(pos, struct playlist_entry, head);
		}
	} else {
		/* add to the end */
		if (!list_empty(&playlist->entries)) {
			after = container_of(playlist->entries.prev, struct playlist_entry, head);
		}
	}
	if (after)
		list_add(&entry->head, &after->head);
	else
		list_add(&entry->head, &playlist->entries);
	ui_add_entry(playlist, after, entry);
	playlist->len++;
	PLAYLIST_UNLOCK(playlist);

	DATABASE_LOCK;
	list_add_tail(&entry->song_head, &song->entries);
	DATABASE_UNLOCK;
	return entry;
}

void remove_playlist(struct playlist *playlist, struct playlist_entry *entry)
{
	PLAYLIST_LOCK(playlist);
	if (entry->playlist) {
		assert(entry->playlist == playlist);
		list_del(&entry->head);
		playlist->len--;
		ui_remove_entry(playlist, entry);
		entry->playlist = NULL;
		put_entry(entry);
	}
	PLAYLIST_UNLOCK(playlist);
}

void clear_playlist(struct playlist *playlist)
{
	struct list_head *pos, *next;

	PLAYLIST_LOCK(playlist);
	list_for_each_safe(pos, next, &playlist->entries) {
		struct playlist_entry *entry
			= container_of(pos, struct playlist_entry, head);
		assert(entry->playlist == playlist);
		ui_remove_entry(playlist, entry);
		entry->playlist = NULL;
		put_entry(entry);
	}
	list_init(&playlist->entries);
	playlist->len = 0;
	PLAYLIST_UNLOCK(playlist);
}

void scan_playlist(struct playlist *playlist)
{
	struct list_head *pos;

	PLAYLIST_LOCK(playlist);
	struct song **table = malloc(sizeof(void *) * playlist->len);
	unsigned int len = 0;
	list_for_each(pos, &playlist->entries) {
		struct playlist_entry *entry =
			container_of(pos, struct playlist_entry, head);
		struct song *song = entry->song;
		get_song(song);
		table[len++] = song;
	}
	PLAYLIST_UNLOCK(playlist);

	unsigned int i;
	for (i = 0; i < len; ++i) {
		get_song_info(table[i]);
		put_song(table[i]);
	}
	free(table);
}

bool save_playlist_m3u(struct playlist *playlist, const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	struct list_head *pos;

	PLAYLIST_LOCK(playlist);
	list_for_each(pos, &playlist->entries) {
		struct playlist_entry *entry =
			container_of(pos, struct playlist_entry, head);
		struct song *song = entry->song;
		fprintf(f, "#EXTINF:");
		if (song->length != (unsigned int) -1)
			fprintf(f, "%d", song->length / 1000);
		else
			fprintf(f, "INVALID");
		if (song->title)
			fprintf(f, ",%s", song->title);
		fprintf(f, "\n%s\n", song->filename);
	}
	PLAYLIST_UNLOCK(playlist);

	fclose(f);
	return true;
}

static size_t song_hash(void *key)
{
	return str_hash(key);
}

static int song_cmp(struct hash_node *node, void *key)
{
	struct song *song = container_of(node, struct song, node);
	return strcmp(song->filename, key) == 0;
}

void init_playlist(void)
{
	pthread_spin_init(&refcountspinlock, 0);
	pthread_mutex_init(&database_mutex, NULL);
	hashmap_init(&song_map, song_hash, song_cmp);
}
