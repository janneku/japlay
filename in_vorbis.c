/*
 * japlay Ogg Vorbis decoder plugin
 * Copyright Janne Kulmala 2010
 */
#include "common.h"
#include "playlist.h"
#include "utils.h"
#include "plugin.h"
#include <vorbis/vorbisfile.h>

struct input_plugin_ctx {
	struct input_state *state;
	OggVorbis_File vf;
	bool reliable;
};

static bool vorbis_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	return ext && !strcasecmp(ext, "ogg");
}

static int vorbis_open(struct input_plugin_ctx *ctx, struct input_state *state,
		       const char *filename)
{
	ctx->state = state;

	FILE *f = fopen(filename, "rb");
	if (!f)
		return -1;

	if (ov_open(f, &ctx->vf, NULL, 0)) {
		fclose(f);
		return -1;
	}

	ctx->reliable = true;

	return 0;
}

static void vorbis_close(struct input_plugin_ctx *ctx)
{
	ov_clear(&ctx->vf);
}

static size_t vorbis_fillbuf(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	while (true) {
		int bitstream;
		long n = ov_read(&ctx->vf, (char *)buffer, maxlen * sizeof(sample_t),
			0, 2, 1, &bitstream);
		if (n == OV_HOLE)
			continue;

		if (n <= 0) {
			set_song_length(get_input_song(ctx->state),
					japlay_get_position(ctx->state),
					ctx->reliable ? 100 : 10);
			return 0;
		}

		vorbis_info *vi = ov_info(&ctx->vf, -1);
		format->rate = vi->rate;
		format->channels = vi->channels;

		return n / sizeof(sample_t);
	}
	return 0;
}

static int vorbis_seek(struct input_plugin_ctx *ctx, struct songpos *newpos)
{
	double s = ((double) newpos->msecs) / 1000.0;
	int ret = ov_time_seek(&ctx->vf, s);
	if (ret < 0) {
		warning("ov_time_seek() error: %d\n", ret);
		return -1;
	}
	ctx->reliable = (newpos->msecs == 0);
	return 1;
}

static struct input_plugin plugin_info = {
	.size = sizeof(struct input_plugin),
	.ctx_size = sizeof(struct input_plugin_ctx),
	.name = "Ogg vorbis decoder",
	.detect = vorbis_detect,
	.open = vorbis_open,
	.close = vorbis_close,
	.fillbuf = vorbis_fillbuf,
	.seek = vorbis_seek,
};

struct input_plugin *get_input_plugin()
{
	return &plugin_info;
}
