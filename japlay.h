#include <glib.h>
#include "list.h"
#include "plugin.h"

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)x

struct song {
	struct list_head head;
	int refcount;
	char *filename;
	void *uidata;
};

/* User interface hooks */
void ui_add_playlist(struct song *song);
void ui_remove_playlist(struct song *song);
void ui_set_playing(struct song *prev, struct song *song);

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
