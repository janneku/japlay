#include <glib.h>
#include "list.h"
#include "plugin.h"
#include "atomic.h"

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)x

struct song;

struct song_ui_ctx;

/* User interface hooks */
extern size_t ui_song_ctx_size;
void ui_add_playlist(struct song *song);
void ui_remove_playlist(struct song *song);
void ui_set_playing(struct song *prev, struct song *song);
void ui_set_power(int power);

/* Getters: */
struct song_ui_ctx *get_song_ui_ctx(struct song *song);
const char *get_song_filename(struct song *song);

struct song *new_song(const char *filename);
void get_song(struct song *song);
void put_song(struct song *song);
struct song *get_playing();

struct input_plugin *detect_plugin(const char *filename);

/* playlist management */
void add_playlist(struct song *song);
void remove_playlist(struct song *song);
void clear_playlist();
void shuffle_playlist();

bool load_playlist_pls(const char *filename);
bool load_playlist_m3u(const char *filename);
bool save_playlist_m3u(const char *filename);

void play_playlist(struct song *song);

void japlay_play();
void japlay_stop();
void japlay_pause();
void japlay_skip();

void japlay_init();
