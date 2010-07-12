#ifndef _JAPLAY_PLUGIN_H_
#define _JAPLAY_PLUGIN_H_

#include <stdbool.h> /* bool */
#include <string.h> /* size_t */

struct input_format {
	unsigned int rate, channels;
};

struct songpos {
	unsigned long msecs;
};

#define MIN_FILL	4096 /* minimum buffer length for plugin fillbuf call */

struct input_state;
struct input_plugin_ctx;

typedef signed short sample_t;

struct input_plugin {
	/* Size of this structure, used for versioning */
	size_t size;

	/* Size of plugin context */
	size_t ctx_size;

	/* Name of the plugin */
	const char *name;

	/* Check if the plugin can handle the given file */
	bool (*detect)(const char *filename);

	/* Try to open the given song file. Context is allocated by the caller.
	   Return -1 if unable to open the file. */
	int (*open)(struct input_plugin_ctx *ctx, struct input_state *state,
		    const char *filename);

	/* Called when file is closed. Context is allocated by the caller */
	void (*close)(struct input_plugin_ctx *ctx);

	/* Fill given buffer with audio samples. Should return the
	   number of samples written to the buffer, and fill all fields
	   in the format structure.
	   Return 0 for EOF or in case of an error. */
	size_t (*fillbuf)(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format);

	/* Return -1 for EOF, 0 if not supported, and 1 if seek successful */
	int (*seek)(struct input_plugin_ctx *ctx, struct songpos *newpos);
};

struct playlist_plugin {
	/* Size of this structure, used for versioning */
	size_t size;

	/* Check if the plugin can handle the given file */
	bool (*detect)(const char *filename);

	/* Name of the plugin */
	const char *name;

	int (*load)(const char *filename);
};

struct input_plugin *get_input_plugin();
struct playlist_plugin *get_playlist_plugin();

/* Getters: */
struct song *get_input_song(struct input_state *state);

/* Call this to get current position in milliseconds */
unsigned int japlay_get_position(struct input_state *state);

#endif
