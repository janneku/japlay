/*
 * uade plugin for japlay
 * Copyright Heikki Orsila <heikki.orsila@iki.fi> 2010
 */
#include <limits.h>
#include <uade/uade.h>

#include "common.h"
#include "playlist.h"
#include "utils.h"
#include "plugin.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static struct uade_state *scanstate;

struct input_plugin_ctx {
	struct uade_state *play;
};

static void init_uade(struct input_plugin_ctx *ctx)
{
	/* Fix me: racy initialization */
	if (scanstate == NULL)
		scanstate = uade_new_state(NULL, NULL);
	if (ctx != NULL && ctx->play == NULL)
		ctx->play = uade_new_state(NULL, NULL);
}

static bool uade_detect(const char *filename)
{
	init_uade(NULL);
	return uade_is_our_file(filename, scanstate);
}

static int uade_open(struct input_plugin_ctx *ctx, struct input_state *state,
		     const char *filename)
{
	UNUSED(state);

	init_uade(ctx);

	if (uade_play(filename, -1, ctx->play)) {
		warning("uade: unable to open file (%s)\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void uade_close(struct input_plugin_ctx *ctx)
{
	uade_stop(ctx->play);
}

static const char *get_fname(struct uade_state *state)
{
	struct uade_song_info info;
	uade_get_song_info(&info, state);
	return info.fname;
}

static size_t uade_fillbuf(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	struct uade_event event;
	size_t len;

	UNUSED(ctx);
	format->channels = 2;
	format->rate = 44100;

	while (1) {
		if (uade_get_event(&event, ctx->play)) {
			fprintf(stderr, "uade_get_event(): error!\n");
			break;
		}

		switch (event.type) {
		case UADE_EVENT_EAGAIN:
			break;
		case UADE_EVENT_DATA:
			len = event.data.size;
			if (maxlen < len) {
				fprintf(stderr, "japlay's fillbuf maxsize is too small. Sound data is discarded!\n");
				len = maxlen;
			}
			memcpy(buffer, event.data.data, len);
			return len / sizeof(sample_t);
		case UADE_EVENT_MESSAGE:
			fprintf(stderr, "uade: %s\n", event.msg);
			break;
		case UADE_EVENT_SONG_END:
			if (!event.songend.happy) {
				fprintf(stderr, "uade: unhappy song end: %s (%s)\n", event.songend.reason, get_fname(ctx->play));
				return 0;
			}
			if (uade_next_subsong(ctx->play))
				return 0;
			break;
		default:
			fprintf(stderr, "uade_get_event returned %s which is not handled.\n", uade_event_name(&event));			
			return 0;
		}
	}
	return 0;
}

static struct input_plugin plugin_info = {
	.size = sizeof(struct input_plugin),
	.ctx_size = sizeof(struct input_plugin_ctx),
	.name = "UADE player",
	.detect = uade_detect,
	.open = uade_open,
	.close = uade_close,
	.fillbuf = uade_fillbuf,
};

struct input_plugin *get_input_plugin()
{
	return &plugin_info;
}
