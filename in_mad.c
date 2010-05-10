#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mad.h>
#include <glib.h>
#include <glib/gprintf.h>

typedef struct mad_context *plugin_ctx_t;
#include "plugin.h"

struct mad_context {
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	int fd;
	unsigned char buffer[8192];
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
static bool mad_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	return !memcmp(filename, "http://", 7) ||
		(ext && !strcasecmp(ext, "mp3"));
}

static struct mad_context *mad_open(const char *filename)
{
	struct mad_context *ctx = g_new(struct mad_context, 1);
	if (!ctx)
		return NULL;

	if (!memcmp(filename, "http://", 7)) {
		size_t i = 7;
		while (filename[i] && filename[i] != '/' && filename[i] != ':' &&
		       !isspace(filename[i]))
			++i;
		char buf[256];
		memcpy(buf, &filename[7], i - 7);
		buf[i - 7] = 0;

		int port = 80;
		if (filename[i] == ':') {
			port = atoi(&filename[i + 1]);
			while (isdigit(filename[i]))
				++i;
		}

		const char *path = "/";
		if (filename[i] == '/')
			path = &filename[i];

		ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
		if (ctx->fd < 0) {
			g_free(ctx);
			return NULL;
		}

		struct sockaddr_in sin;
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = inet_addr(buf);
		sin.sin_port = htons(port);

		if (connect(ctx->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			g_printf("unable to connect: %s\n", strerror(errno));
			close(ctx->fd);
			g_free(ctx);
			return NULL;
		}

		sprintf(buf, "GET %s HTTP/1.0\r\n\r\n", path);
		write(ctx->fd, buf, strlen(buf));
	} else {
		ctx->fd = open(filename, O_RDONLY);
		if (ctx->fd < 0) {
			g_printf("unable to open file: %s\n", strerror(errno));
			g_free(ctx);
			return NULL;
		}
	}

	mad_frame_init(&ctx->frame);
	mad_stream_init(&ctx->stream);
	mad_synth_init(&ctx->synth);

	return ctx;
}

static void mad_close(struct mad_context *ctx)
{
	mad_frame_finish(&ctx->frame);
	mad_stream_finish(&ctx->stream);
	mad_synth_finish(&ctx->synth);
	close(ctx->fd);
	g_free(ctx);
}

static sample_t scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static size_t mad_fillbuf(struct mad_context *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	while (true) {
		size_t len = 0;
		if (ctx->stream.next_frame) {
			len = ctx->stream.bufend - ctx->stream.next_frame;
			if (len)
				memcpy(ctx->buffer, ctx->stream.next_frame, len);
		}
		ssize_t ret = read(ctx->fd, &ctx->buffer[len], sizeof(ctx->buffer) - len);
		if (ret < 0) {
			g_printf("read error: %s\n", strerror(errno));
			return 0;
		}
		len += ret;

		mad_stream_buffer(&ctx->stream, ctx->buffer, len);

		if (mad_header_decode(&ctx->frame.header, &ctx->stream)) {
			if (ctx->stream.error == MAD_ERROR_BUFLEN)
				return 0;
			g_printf("MAD error: %s\n", mad_stream_errorstr(&ctx->stream));
			continue;
		}

		if (mad_frame_decode(&ctx->frame, &ctx->stream)) {
			if (ctx->stream.error == MAD_ERROR_BUFLEN)
				return 0;
			g_printf("MAD error: %s\n", mad_stream_errorstr(&ctx->stream));
			continue;
		}

		format->rate = ctx->frame.header.samplerate;
		format->channels = MAD_NCHANNELS(&ctx->frame.header);

		mad_synth_frame(&ctx->synth, &ctx->frame);

		len = ctx->synth.pcm.length * format->channels;

		if (len > maxlen) {
			g_printf("Too small buffer!\n");
			return 0;
		}

		if (format->channels == 2) {
			size_t i;
			for (i = 0; i < ctx->synth.pcm.length; ++i) {
				buffer[i * 2] = scale(ctx->synth.pcm.samples[0][i]);
				buffer[i * 2 + 1] = scale(ctx->synth.pcm.samples[1][i]);
			}
		} else if (format->channels == 1) {
			size_t i;
			for (i = 0; i < ctx->synth.pcm.length; ++i)
				buffer[i] = scale(ctx->synth.pcm.samples[0][i]);
		}
		return len;
	}
}

static const struct input_plugin plugin_info = {
	sizeof(struct input_plugin),
	"libmad MPEG audio decoder",
	mad_detect,
	mad_open,
	mad_close,
	mad_fillbuf,
};

const struct input_plugin *get_info()
{
	return &plugin_info;
}
