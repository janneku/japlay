#ifndef _JAPLAY_H_
#define _JAPLAY_H_

#include <stdbool.h> /* bool */

struct song;
struct input_plugin;

struct song *get_playing(void);

struct input_plugin *detect_plugin(const char *filename);

void add_file_playlist(const char *filename);

bool load_playlist_pls(const char *filename);
bool load_playlist_m3u(const char *filename);

void play_playlist(struct song *song);

void japlay_play(void);
void japlay_seek_relative(long msecs);
void japlay_stop(void);
void japlay_pause(void);
void japlay_skip(void);

int japlay_connect(void);
void japlay_send(int fd, const char *filename);

int japlay_init(void);
void japlay_exit(void);

#endif
