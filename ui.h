#ifndef _JAPLAY_UI_H_
#define _JAPLAY_UI_H_

#include <string.h> /* size_t */

/* User interface prototype */

#define SCOPE_SIZE	256  /* enough for 50 KHz, stereo */

struct song;
struct entry_ui_ctx;
struct playlist;
struct playlist_ui_ctx;
struct playlist_entry;

extern size_t ui_song_ctx_size;
extern size_t ui_playlist_ctx_size;
void ui_add_entry(struct playlist *playlist, struct playlist_entry *after,
		  struct playlist_entry *entry);
void ui_error(const char *fmt, ...);
void ui_remove_entry(struct playlist *playlist, struct playlist_entry *entry);
void ui_init_playlist(struct playlist *playlist);
void ui_hide_playlist(struct playlist *playlist);
void ui_update_entry(struct playlist *playlist, struct playlist_entry *entry);
void ui_set_cursor(struct playlist_entry *entry);
void ui_set_status(int *scope, size_t scope_len, unsigned int position);
void ui_set_streaming_title(const char *title);
void ui_show_message(const char *fmt, ...);

#endif
