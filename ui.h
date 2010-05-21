#include <string.h> /* size_t */

struct song;

struct song_ui_ctx;

/* User interface hooks */
extern size_t ui_song_ctx_size;
void ui_add_playlist(struct song *song);
void ui_remove_playlist(struct song *song);
void ui_set_playing(struct song *prev, struct song *song);
void ui_set_power(int power);

