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
#include <glib/gprintf.h>

typedef struct vorbis_context *plugin_ctx_t;
#include "plugin.h"

struct vorbis_context {
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

static struct vorbis_context *vorbis_open(const char *filename)
{
	struct vorbis_context *ctx = g_new(struct vorbis_context, 1);
	if (!ctx)
		return NULL;

	FILE *f = fopen(filename, "rb");
	if (!f) {
		g_free(ctx);
		return NULL;
	}

	if (ov_open(f, &ctx->vf, NULL, 0)) {
		fclose(f);
		g_free(ctx);
		return NULL;
	}

	return ctx;
}

static void vorbis_close(struct vorbis_context *ctx)
{
	ov_clear(&ctx->vf);
	g_free(ctx);
}

static size_t vorbis_fillbuf(struct vorbis_context *ctx, sample_t *buffer,
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
