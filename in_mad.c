/*
 * japlay mikmod MPEG audio decoder plugin
 * Copyright Janne Kulmala 2010
 */
#include "plugin.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <assert.h>

#include <mad.h>

struct input_plugin_ctx {
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	int fd;
	unsigned char buffer[8192];
	size_t *seconds;
	size_t nseconds;
	bool reliable;
};

#define DEFAULT_BYTE_RATE (128000 / 8)
#define MAX_SECS (365 * 24 * 3600)     /* a year :-) */

static void remember(struct input_plugin_ctx *ctx, size_t fpos, size_t t)
{
	size_t *seconds;
	size_t nseconds;
	size_t i;

	if (t >= MAX_SECS)
		return;

	if (t >= ctx->nseconds) {
		nseconds = (t == 0) ? 8 : 2 * t;
		seconds = realloc(ctx->seconds, nseconds * sizeof(ctx->seconds[0]));
		if (seconds == NULL)
			return;
		for (i = ctx->nseconds; i < nseconds; i++)
			seconds[i] = 0;
		ctx->seconds = seconds;
		ctx->nseconds = nseconds;
	}

	if (t == 0 || ctx->seconds[t])
		return;

	ctx->seconds[t] = fpos;
}

static size_t recall(struct input_plugin_ctx *ctx, size_t t)
{
	if (t < ctx->nseconds && ctx->seconds[t])
		return ctx->seconds[t];
	return 0;
}

static size_t estimate(struct input_plugin_ctx *ctx, size_t t)
{
	size_t lo_fpos = 0;
	size_t lo_t = 0;
	size_t hi_fpos = 0;
	size_t hi_t = 0;
	size_t newpos;
	size_t avg;

	if (t == 0)
		return 0;

	newpos = recall(ctx, t);
	if (newpos > 0)
		return newpos;

	if (t < ctx->nseconds) {
		/* Find previous non-zero offset */
		for (lo_t = t; lo_t > 0; lo_t--) {
			lo_fpos = ctx->seconds[lo_t];
			if (lo_fpos)
				break;
		}

		/* Find the next non-zero offset */
		for (hi_t = t + 1; hi_t < ctx->nseconds; hi_t++) {
			hi_fpos = ctx->seconds[hi_t];
			if (hi_fpos)
				break;
		}
		if (hi_t == ctx->nseconds)
			hi_t = 0;

	} else if (ctx->nseconds > 0) {
		/* Find last non-zero offset */
		for (hi_t = ctx->nseconds - 1; hi_t > 0; hi_t--) {
			hi_fpos = ctx->seconds[hi_t];
			if (hi_fpos)
				break;
		}
	}

	if (lo_t <= t && t < hi_t) {
		/* Interpolate between lo_t and hi_t points */
		avg = (hi_fpos - lo_fpos) / (hi_t - lo_t);
		newpos = lo_fpos + (t - lo_t) * avg;
	} else if (hi_t > lo_t) {
		/* Extrapolate over hi_t. avg calculated from lo_t and hi_t. */
		avg = (hi_fpos - lo_fpos) / (hi_t - lo_t);
		newpos = hi_fpos + (t - hi_t) * avg;
	} else {
		/* No average, make a quess. hi_t == hi_fpos == 0 is OK. */
		avg = DEFAULT_BYTE_RATE;
		newpos = hi_fpos + avg * (t - hi_t);
	}

	return newpos;
}

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

static int mad_open(struct input_plugin_ctx *ctx, const char *filename)
{
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
			++i;
			port = atoi(&filename[i]);
			while (isdigit(filename[i]))
				++i;
		}

		const char *path = "/";
		if (filename[i] == '/')
			path = &filename[i];

		in_addr_t addr = inet_addr(buf);
		if (addr == (in_addr_t)-1) {
			struct hostent *hp = gethostbyname(buf);
			if (!hp)
				return -1;
			addr = ((struct in_addr *)hp->h_addr_list[0])->s_addr;
		}

		ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
		if (ctx->fd < 0)
			return -1;

		struct sockaddr_in sin;
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = addr;
		sin.sin_port = htons(port);

		if (connect(ctx->fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			printf("unable to connect (%s)\n", strerror(errno));
			close(ctx->fd);
			return -1;
		}

		sprintf(buf, "GET %s HTTP/1.0\r\n\r\n", path);
		write(ctx->fd, buf, strlen(buf));
	} else {
		ctx->fd = open(filename, O_RDONLY);
		if (ctx->fd < 0) {
			printf("unable to open file (%s)\n", strerror(errno));
			return -1;
		}
	}

	mad_frame_init(&ctx->frame);
	mad_stream_init(&ctx->stream);
	mad_synth_init(&ctx->synth);

	ctx->seconds = NULL;
	ctx->nseconds = 0;
	ctx->reliable = true;

	return 0;
}

static void mad_close(struct input_plugin_ctx *ctx)
{
	mad_frame_finish(&ctx->frame);
	mad_stream_finish(&ctx->stream);
	mad_synth_finish(&ctx->synth);
	close(ctx->fd);
	free(ctx->seconds);
	ctx->seconds = NULL;
	ctx->nseconds = 0;
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

static void print_mad_error(const struct mad_stream *stream)
{
	if (stream->error == MAD_ERROR_NONE || MAD_RECOVERABLE(stream->error))
		return;
	printf("MAD error: %s\n", mad_stream_errorstr(stream));
}

static size_t mad_fillbuf(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	size_t t;
	while (true) {
		size_t len = 0;
		off_t offs;

		if (ctx->stream.next_frame) {
			len = ctx->stream.bufend - ctx->stream.next_frame;
			if (len)
				memcpy(ctx->buffer, ctx->stream.next_frame, len);
		}
		ssize_t ret = read(ctx->fd, &ctx->buffer[len], sizeof(ctx->buffer) - len);
		if (ret < 0) {
			printf("read failed (%s)\n", strerror(errno));
			return 0;
		}
		len += ret;

		offs = lseek(ctx->fd, 0, SEEK_CUR);
		if (offs == -1)
			offs = 0;
		if ((size_t) offs >= len)
			offs -= len;

		mad_stream_buffer(&ctx->stream, ctx->buffer, len);

		if (mad_header_decode(&ctx->frame.header, &ctx->stream)) {
			if (ctx->stream.error == MAD_ERROR_BUFLEN) {
				japlay_set_song_length(japlay_get_position(), ctx->reliable);
				return 0;
			}
			print_mad_error(&ctx->stream);
			continue;
		}

		if (mad_frame_decode(&ctx->frame, &ctx->stream)) {
			print_mad_error(&ctx->stream);
			continue;
		}

		format->rate = ctx->frame.header.samplerate;
		format->channels = MAD_NCHANNELS(&ctx->frame.header);

		mad_synth_frame(&ctx->synth, &ctx->frame);

		len = ctx->synth.pcm.length * format->channels;

		if (len > maxlen) {
			printf("Too small buffer!\n");
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

		t = japlay_get_position() / 1000;
		if (!recall(ctx, t))
			remember(ctx, offs, t);
		return len;
	}
}

static int mad_seek(struct input_plugin_ctx *ctx, struct songpos *newpos)
{
	size_t offs;
	size_t curt;
	size_t t = newpos->msecs / 1000;

	curt = japlay_get_position() / 1000;
	if (t == curt)
		return 1;

	offs = recall(ctx, t);
	if (!offs)
		offs = estimate(ctx, t);

	lseek(ctx->fd, offs, SEEK_SET);

	newpos->msecs = 1000 * t;

	mad_frame_mute(&ctx->frame);
	mad_synth_mute(&ctx->synth);
	mad_stream_finish(&ctx->stream);
	mad_stream_init(&ctx->stream);

	ctx->reliable = false;

	return 1;
}

static struct input_plugin plugin_info = {
	.size = sizeof(struct input_plugin),
	.ctx_size = sizeof(struct input_plugin_ctx),
	.name = "libmad MPEG audio decoder",
	.detect = mad_detect,
	.open = mad_open,
	.close = mad_close,
	.fillbuf = mad_fillbuf,
	.seek = mad_seek,
};


struct input_plugin *get_info()
{
	return &plugin_info;
}
