#ifndef _PLAYLIST_H_
#define _PLAYLIST_H_

#include <stdbool.h> /* bool */

struct song;
struct song_ui_ctx;

/* Getters: */
struct song_ui_ctx *get_song_ui_ctx(struct song *song);
const char *get_song_filename(struct song *song);

struct song *new_song(const char *filename);
void get_song(struct song *song);
void put_song(struct song *song);
struct song *playlist_next(struct song *song, bool forward);
struct song *get_playlist_first(void);
void add_playlist(struct song *song);
void remove_playlist(struct song *song);
void clear_playlist(void);
void shuffle_playlist(void);
bool save_playlist_m3u(const char *filename);
void init_playlist(void);

#endif
