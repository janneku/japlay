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
#include <glib.h>
#include <stdlib.h>
#include <pthread.h>

static struct list_head playlist;
static struct hashmap song_map;
static unsigned int playlist_len = 0;

static pthread_spinlock_t refcountspinlock;
static pthread_mutex_t database_mutex;

#define REF_COUNT_LOCK pthread_spin_lock(&refcountspinlock)
#define REF_COUNT_UNLOCK pthread_spin_unlock(&refcountspinlock)

#define DATABASE_LOCK pthread_mutex_lock(&database_mutex)
#define DATABASE_UNLOCK pthread_mutex_unlock(&database_mutex)

struct song {
	struct hash_node node;
	int length_score;
	unsigned int refcount, length;
	char *filename, *title;
};

struct playlist_entry {
	struct list_head head;
	unsigned int refcount;
	struct song *song;
	struct song_ui_ctx *ui_ctx;
};

struct song *get_entry_song(struct playlist_entry *entry)
{
	/* NOTE: this does NOT increase refcount! */
	return entry->song;
}

struct song_ui_ctx *get_entry_ui_ctx(struct playlist_entry *entry)
{
	return entry->ui_ctx;
}

const char *get_song_filename(struct song *song)
{
	return song->filename;
}

char *get_song_title(struct song *song)
{
	if (song->title)
		return strdup(song->title);
	return NULL;
}

unsigned int get_song_length(struct song *song)
{
	return song->length;
}

static struct song *find_song_locked(const char *filename)
{
	struct hash_node *node = hashmap_get(&song_map, (void *)filename);
	if (node == NULL)
		return NULL;
	printf("hashmap hit\n");
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

struct song *new_song(const char *filename)
{
	DATABASE_LOCK;
	struct song *song = find_song_locked(filename);
	if (song == NULL) {
		song = NEW(struct song);
		if (song) {
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
	song->refcount++;
	REF_COUNT_UNLOCK;
}

void put_song(struct song *song)
{
	REF_COUNT_LOCK;
	song->refcount--;
	bool zero = (song->refcount == 0);
	REF_COUNT_UNLOCK;

	if (zero) {
		free(song->filename);
		if (song->title)
			free(song->title);
		DATABASE_LOCK;
		hashmap_remove(&song_map, (void *)song->filename);
		DATABASE_UNLOCK;
		free(song);
	}
}

void get_entry(struct playlist_entry *entry)
{
	REF_COUNT_LOCK;
	entry->refcount++;
	REF_COUNT_UNLOCK;
}

void put_entry(struct playlist_entry *entry)
{
	REF_COUNT_LOCK;
	entry->refcount--;
	bool zero = (entry->refcount == 0);
	REF_COUNT_UNLOCK;

	if (zero) {
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
		/*FIXME
		ui_update_playlist(song);*/
	}
}

void set_song_title(struct song *song, const char *str)
{
	/* TODO: locking */
	if (song->title)
		free(song->title);
	song->title = strdup(str);
	/*FIXME
	ui_update_playlist(song);*/
}

struct playlist_entry *playlist_next(struct playlist_entry *entry, bool forward)
{
	struct playlist_entry *next = NULL;
	DATABASE_LOCK;
	if (entry->head.next) {
		struct list_head *pos;
		if (forward)
			pos = entry->head.next;
		else
			pos = entry->head.prev;
		if (pos != &playlist) {
			next = container_of(pos, struct playlist_entry, head);
			get_entry(next);
		}
	}
	DATABASE_UNLOCK;
	return next;
}

struct playlist_entry *get_playlist_first(void)
{
	DATABASE_LOCK;
	struct playlist_entry *entry = NULL;
	if (!list_empty(&playlist)) {
		entry = container_of(playlist.next, struct playlist_entry, head);
		get_entry(entry);
	}
	DATABASE_UNLOCK;

	return entry;
}

struct playlist_entry *add_playlist(struct song *song)
{
	struct playlist_entry *entry = NEW(struct playlist_entry);
	if (entry == NULL)
		return NULL;
	entry->ui_ctx = malloc(ui_song_ctx_size);
	entry->refcount = 2; /* yes, return with refcount of two */
	get_song(song);
	entry->song = song;

	DATABASE_LOCK;
	list_add_tail(&entry->head, &playlist);
	ui_add_playlist(entry);
	playlist_len++;
	DATABASE_UNLOCK;
	return entry;
}

void remove_playlist(struct playlist_entry *entry)
{
	DATABASE_LOCK;
	list_del(&entry->head);
	playlist_len--;
	memset(&entry->head, 0, sizeof(entry->head));
	ui_remove_playlist(entry);
	DATABASE_UNLOCK;

	put_entry(entry);
}

void clear_playlist(void)
{
	struct list_head *pos, *next;

	DATABASE_LOCK;
	list_for_each_safe(pos, next, &playlist) {
		struct playlist_entry *entry
			= container_of(pos, struct playlist_entry, head);
		memset(&entry->head, 0, sizeof(entry->head));
		ui_remove_playlist(entry);
		put_entry(entry);
	}
	list_init(&playlist);
	playlist_len = 0;
	DATABASE_UNLOCK;
}

void shuffle_playlist(void)
{
	struct list_head *pos;

	DATABASE_LOCK;
	struct playlist_entry **table = malloc(sizeof(void *) * playlist_len);
	unsigned int len = 0;
	list_for_each(pos, &playlist) {
		struct playlist_entry *entry =
			container_of(pos, struct playlist_entry, head);
		unsigned int i = rand() % (len + 1);
		if (i != len)
			memmove(&table[i + 1], &table[i], (len - i) * sizeof(table[0]));
		table[i] = entry;
		ui_remove_playlist(entry);
		len++;
	}
	list_init(&playlist);
	unsigned int i;
	for (i = 0; i < playlist_len; ++i) {
		struct playlist_entry *entry = table[i];
		list_add_tail(&entry->head, &playlist);
		ui_add_playlist(entry);
	}
	free(table);
	DATABASE_UNLOCK;
}

void scan_playlist(void)
{
	struct list_head *pos;

	DATABASE_LOCK;
	struct song **table = malloc(sizeof(void *) * playlist_len);
	unsigned int len = 0;
	list_for_each(pos, &playlist) {
		struct playlist_entry *entry =
			container_of(pos, struct playlist_entry, head);
		struct song *song = entry->song;
		get_song(song);
		table[len++] = song;
	}
	DATABASE_UNLOCK;

	unsigned int i;
	for (i = 0; i < len; ++i) {
		get_song_info(table[i]);
		put_song(table[i]);
	}
	free(table);
}

bool save_playlist_m3u(const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	struct list_head *pos;

	DATABASE_LOCK;
	list_for_each(pos, &playlist) {
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
	DATABASE_UNLOCK;

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
	list_init(&playlist);
	hashmap_init(&song_map, song_hash, song_cmp);
}
