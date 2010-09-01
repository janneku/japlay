#ifndef _JAPLAY_H_
#define _JAPLAY_H_

#include <stdbool.h> /* bool */

struct song;
struct playlist_entry;
struct playlist;

struct playlist_entry *get_cursor(void);

extern struct playlist *japlay_queue, *japlay_history;

int get_song_info(struct song *song);

struct playlist_entry *add_file_playlist(struct playlist *playlist,
					 const char *filename);
int add_dir_or_file_playlist(struct playlist *playlist, const char *path);

int load_playlist(struct playlist *playlist, const char *filename);

void japlay_play(void);
void japlay_set_autovol(bool enabled);
void japlay_seek_relative(long msecs);
void japlay_seek(long msecs);
void japlay_stop(void);
void japlay_pause(void);
void japlay_skip(void);

int japlay_init(int *argc, char **argv);
void japlay_exit(void);

#endif
