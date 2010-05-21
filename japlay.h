#include <stdbool.h> /* bool */

#define APP_NAME		"japlay"

struct song;
struct input_plugin;

struct song *get_playing();

struct input_plugin *detect_plugin(const char *filename);

bool load_playlist_pls(const char *filename);
bool load_playlist_m3u(const char *filename);

void play_playlist(struct song *song);

void japlay_play();
void japlay_stop();
void japlay_pause();
void japlay_skip();

void japlay_init();
