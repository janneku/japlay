#ifndef _JAPLAY_UI_H_
#define _JAPLAY_UI_H_

#include <string.h> /* size_t */

/* User interface prototype */

struct song;

struct song_ui_ctx;

extern size_t ui_song_ctx_size;
void ui_add_playlist(struct song *song);
void ui_remove_playlist(struct song *song);
void ui_update_playlist(struct song *song);
void ui_set_cursor(struct song *prev, struct song *song);
void ui_set_status(int power, unsigned int position);
void ui_set_streaming_title(const char *title);
void ui_show_message(const char *msg);

#endif
