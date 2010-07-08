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

	/* Try to open the given file and initialize the plugin. Should
	   return a pointer to internal decoder context for the file.
	   Returns NULL in case of a failure. */
	bool (*open)(struct input_plugin_ctx *ctx, const char *filename);

	/* Called when file is closed. Should free memory allocated
	   for the file context. */
	void (*close)(struct input_plugin_ctx *ctx);

	/* Fill given buffer with decoded audio data. Should return the
	   number of samples written to the buffer, and fill all fields
	   of the format structure. */
	size_t (*fillbuf)(struct input_plugin_ctx *ctx, sample_t *buffer, size_t maxlen,
			  struct input_format *format);

	/* Return -1 for EOF, 0 if not supported, and 1 if seek successful */
	int (*seek)(struct input_plugin_ctx *ctx,
		    struct songpos *newpos);

	/* Call this to get current position in milliseconds */
	unsigned int (*get_position)(void);
};

struct input_plugin *get_info();

#endif
