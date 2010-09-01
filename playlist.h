#ifndef _PLAYLIST_H_
#define _PLAYLIST_H_

#include <stdbool.h> /* bool */

struct song;
struct entry_ui_ctx;
struct playlist;
struct playlist_ui_ctx;
struct playlist_entry;

/* Getters: */
struct song *get_entry_song(struct playlist_entry *entry);
struct entry_ui_ctx *get_entry_ui_ctx(struct playlist_entry *entry);
struct playlist_ui_ctx *get_playlist_ui_ctx(struct playlist *playlist);
const char *get_song_filename(struct song *song);
char *get_song_title(struct song *song);
unsigned int get_song_length(struct song *song);

void set_playlist_shuffle(struct playlist *playlist, bool enabled);
struct song *find_song(const char *filename);
struct playlist *new_playlist(void);
struct song *new_song(const char *filename);
void get_song(struct song *song);
void put_song(struct song *song);
void get_entry(struct playlist_entry *entry);
void put_entry(struct playlist_entry *entry);
void set_song_length(struct song *song, unsigned int length, int score);
void set_song_title(struct song *song, const char *str);
struct playlist_entry *get_playlist_first(struct playlist *playlist);
struct playlist_entry *add_playlist(struct playlist *playlist, struct song *song,
				    bool first);
void remove_playlist(struct playlist *playlist, struct playlist_entry *entry);
void clear_playlist(struct playlist *playlist);
void shuffle_playlist(struct playlist *playlist);
void scan_playlist(struct playlist *playlist);
bool save_playlist_m3u(struct playlist *playlist, const char *filename);
void init_playlist(void);

#endif
