#ifndef _PLAYLIST_H_
#define _PLAYLIST_H_

#include <stdbool.h> /* bool */

struct song;
struct song_ui_ctx;
struct playlist_entry;

/* Getters: */
struct song *get_entry_song(struct playlist_entry *entry);
struct song_ui_ctx *get_entry_ui_ctx(struct playlist_entry *entry);
const char *get_song_filename(struct song *song);
char *get_song_title(struct song *song);
unsigned int get_song_length(struct song *song);

struct song *find_song(const char *filename);
struct song *new_song(const char *filename);
void get_song(struct song *song);
void put_song(struct song *song);
void get_entry(struct playlist_entry *entry);
void put_entry(struct playlist_entry *entry);
void set_song_length(struct song *song, unsigned int length, int score);
void set_song_title(struct song *song, const char *str);
struct playlist_entry *playlist_next(struct playlist_entry *entry, bool forward);
struct playlist_entry *get_playlist_first(void);
struct playlist_entry *add_playlist(struct song *song);
void remove_playlist(struct playlist_entry *entry);
void clear_playlist(void);
void shuffle_playlist(void);
void scan_playlist(void);
bool save_playlist_m3u(const char *filename);
void init_playlist(void);

#endif
