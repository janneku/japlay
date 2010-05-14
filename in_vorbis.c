/*
 * japlay Ogg Vorbis decoder plugin
 * Copyright Janne Kulmala 2010
 */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <vorbis/vorbisfile.h>
#include <glib.h>
#include "plugin.h"

struct input_plugin_ctx {
	OggVorbis_File vf;
};

static const char *file_ext(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/') {
		if (filename[i - 1] == '.')
			return &filename[i];
		--i;
	}
	return NULL;
}
static bool vorbis_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	return ext && !strcasecmp(ext, "ogg");
}

static bool vorbis_open(struct input_plugin_ctx *ctx, const char *filename)
{
	FILE *f = fopen(filename, "rb");
	if (!f)
		return false;

	if (ov_open(f, &ctx->vf, NULL, 0)) {
		fclose(f);
		return false;
	}

	return true;
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

		if (n <= 0)
			return 0;

		vorbis_info *vi = ov_info(&ctx->vf, -1);
		format->rate = vi->rate;
		format->channels = vi->channels;

		return n / sizeof(sample_t);
	}
	return 0;
}

static const struct input_plugin plugin_info = {
	sizeof(struct input_plugin),
	sizeof(struct input_plugin_ctx),
	"Ogg vorbis decoder",
	vorbis_detect,
	vorbis_open,
	vorbis_close,
	vorbis_fillbuf,
};

const struct input_plugin *get_info()
{
	return &plugin_info;
}
