/*
 * japlay libmad MPEG audio decoder plugin
 * Copyright Janne Kulmala 2010
 */
#define _GNU_SOURCE

#include "plugin.h"
#include "playlist.h"
#include "common.h"
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
	struct input_state *state;
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	int fd;
	bool eof;
	size_t fpos, length;
	size_t *seconds;
	size_t nseconds;
	bool reliable, streaming;

	/* read buffer */
	unsigned char buffer[8192];
	size_t buflen;

	/* URL */
	char *host;
	const char *path;
	int port;
	in_addr_t addr;

	/* HTTP stream metadata */
	size_t metainterval, metapos;
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

static unsigned int estimate_length(struct input_plugin_ctx *ctx)
{
	size_t lo_fpos = 0;
	size_t lo_t = 0;
	size_t hi_fpos = 0;
	size_t hi_t = 0;
	size_t avg;
	size_t length;

	if (ctx->nseconds > 0) {
		/* Find last non-zero offset */
		for (hi_t = ctx->nseconds - 1; hi_t > 0; hi_t--) {
			hi_fpos = ctx->seconds[hi_t];
			if (hi_fpos)
				break;
		}
	}
	if (hi_fpos > lo_fpos) {
		avg = (hi_fpos - lo_fpos) / (hi_t - lo_t);
		length = hi_t + (ctx->length - hi_fpos) / avg;
	} else {
		avg = DEFAULT_BYTE_RATE;
		length = hi_t + (ctx->length - hi_fpos) / avg;
	}

	return length;
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

static int setblocking(int fd, bool blocking)
{
	int flags = fcntl(fd, F_GETFL);
	flags &= ~O_NONBLOCK;
	if (!blocking)
		flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

static int wait_on_socket(int fd, bool for_recv, int timeout_ms)
{
	struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};

	fd_set infd, outfd;
	FD_ZERO(&infd);
	FD_ZERO(&outfd);
	if (for_recv)
		FD_SET(fd, &infd);
	else
		FD_SET(fd, &outfd);

	while (true) {
		int ret = select(fd + 1, &infd, &outfd, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			warning("select failed (%s)\n", strerror(errno));
			return -1;
		} else if (ret == 0) {
			warning("connection timeout\n");
			return -1;
		}
		break;
	}
	return 0;
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

static size_t xread(int fd, void *buf, size_t maxlen)
{
	while (true) {
		ssize_t ret = read(fd, buf, maxlen);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			warning("read failed (%s)\n", strerror(errno));
			ret = 0;
		}
		return ret;
	}
}

/* fill the read buffer */
static int fillbuf(struct input_plugin_ctx *ctx)
{
	if (ctx->streaming) {
		if (wait_on_socket(ctx->fd, true, 5000)) {
			ctx->eof = true;
			return -1;
		}
	}
	size_t len = xread(ctx->fd, &ctx->buffer[ctx->buflen],
			   sizeof(ctx->buffer) - ctx->buflen);
	if (len == 0) {
		ctx->eof = true;
		return -1;
	}
	ctx->buflen += len;
	return 0;
}

static void skipbuf(struct input_plugin_ctx *ctx, size_t pos, size_t len)
{
	ctx->buflen -= len;
	memmove(&ctx->buffer[pos], &ctx->buffer[pos + len], ctx->buflen - pos);
}

static char *trim(char *buf)
{
	size_t i = strlen(buf);
	while (i && isspace(buf[i - 1]))
		--i;
	buf[i] = 0;
	i = 0;
	while (isspace(buf[i]))
		++i;
	return &buf[i];
}

static int parse_url(struct input_plugin_ctx *ctx, const char *url)
{
	size_t i = 0;
	while (url[i] && url[i] != '/' && url[i] != ':' && !isspace(url[i]))
		++i;
	ctx->host = strndup(url, i);

	ctx->addr = inet_addr(ctx->host);
	if (ctx->addr == (in_addr_t) -1) {
		struct hostent *hp = gethostbyname(ctx->host);
		if (!hp) {
			warning("unable to resolve host %s\n", ctx->host);
			free(ctx->host);
			return -1;
		}
		ctx->addr = ((struct in_addr *) hp->h_addr_list[0])->s_addr;
	}

	ctx->port = 80;
	if (url[i] == ':') {
		++i;
		ctx->port = atoi(&url[i]);
		if (ctx->port <= 0 || ctx->port >= 0x10000)
			return -1;
		while (isdigit(url[i]))
			++i;
	}

	ctx->path = "/";
	if (url[i] == '/')
		ctx->path = &url[i];

	return 0;
}

static int connect_http(struct input_plugin_ctx *ctx, size_t offset)
{
	ctx->fd = -1;
	ctx->streaming = true;
	ctx->buflen = 0;
	ctx->metainterval = 0;

	ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (ctx->fd < 0)
		goto err;

	setblocking(ctx->fd, false);

	struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ctx->addr;
	sin.sin_port = htons(ctx->port);

	if (connect(ctx->fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		if (errno != EINPROGRESS) {
			warning("unable to connect to %s (%s)\n", ctx->host,
				strerror(errno));
			goto err;
		}
	}

	if (wait_on_socket(ctx->fd, false, 5000)) {
		warning("connection timeout\n");
		goto err;
	}

	char *range;
	if (offset) {
		if (asprintf(&range, "Range: bytes=%zd-%zd\r\n",
				     offset, ctx->length-1) < 0)
			goto err;
	} else
		range = strdup("");

	/* send HTTP request */
	char *req;
	if (asprintf(&req, "GET %s HTTP/1.1\r\n"
			   "Host: %s\r\n"
			   "Icy-MetaData:1\r\n"
			   "%s"
			   "User-Agent: japlay/1.0\r\n\r\n", ctx->path, ctx->host, range) < 0)
		goto err;
	free(range);

	size_t len = strlen(req);
	if (write(ctx->fd, req, len) < (ssize_t)len) {
		free(req);
		goto err;
	}
	free(req);

	/* parse HTTP reponse */
	bool done = false;
	while (!done) {
		/* try to read one line */
		unsigned char *newline;
		while (true) {
			newline = memchr(ctx->buffer, '\n', ctx->buflen);
			if (newline)
				break;
			if (ctx->eof)
				goto err;
			if (fillbuf(ctx))
				goto err;
		}
		*newline = 0;
		char *line = trim((char *) ctx->buffer);

		info("HTTP: %s\n", line);

		const char contype[] = "content-type:";
		const char conlen[] = "content-length:";
		const char title[] = "icy-name:";
		const char metaint[] = "icy-metaint:";

		if (!strncasecmp(line, contype, strlen(contype))) {
			char *value = trim(&line[strlen(contype)]);
			if (strcmp(value, "audio/mpeg")) {
				warning("invalid content type: %s\n", value);
				goto err;
			}
		} else if (!strncasecmp(line, conlen, strlen(conlen)) && offset == 0) {
			ctx->length = atol(&line[strlen(conlen)]);
		} else if (!strncasecmp(line, title, strlen(title))) {
			set_song_title(get_input_song(ctx->state), trim(&line[strlen(title)]));
		} else if (!strncasecmp(line, metaint, strlen(metaint))) {
			ctx->metainterval = atol(&line[strlen(metaint)]);
		} else if (*line == 0)
			done = true;

		skipbuf(ctx, 0, newline - ctx->buffer + 1);
	}
	ctx->metapos = ctx->metainterval;
	return 0;

 err:
	if (ctx->fd >= 0)
		close(ctx->fd);
	return -1;
}

static int mad_open(struct input_plugin_ctx *ctx, struct input_state *state,
		    const char *filename)
{
	/* ctx is zeroed by the caller */

	ctx->state = state;
	ctx->length = (size_t) -1;

	if (!memcmp(filename, "http://", 7)) {
		if (parse_url(ctx, &filename[7]))
			return -1;
		if (connect_http(ctx, 0))
			return -1;
	} else {
		ctx->fd = open(filename, O_RDONLY);
		if (ctx->fd < 0) {
			warning("unable to open file (%s)\n", strerror(errno));
			return -1;
		}
		ctx->length = lseek(ctx->fd, 0, SEEK_END);
		lseek(ctx->fd, 0, SEEK_SET);

		if (fillbuf(ctx)) {
			close(ctx->fd);
			return -1;
		}
	}

	mad_frame_init(&ctx->frame);
	mad_stream_init(&ctx->stream);
	mad_synth_init(&ctx->synth);

	ctx->reliable = true;

	return 0;
}

static void mad_close(struct input_plugin_ctx *ctx)
{
	if (ctx->length != (size_t) -1) {
		set_song_length(get_input_song(ctx->state),
				estimate_length(ctx) * 1000, 10);
	}
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
	warning("MAD error: %s\n", mad_stream_errorstr(stream));
}

static int read_meta(struct input_plugin_ctx *ctx)
{
	size_t metalen = ctx->buffer[ctx->metapos] * 16 + 1;
	char meta[4096];

	size_t i = ctx->buflen - ctx->metapos;
	if (i > metalen)
		i = metalen;
	memcpy(meta, &ctx->buffer[ctx->metapos], i);
	skipbuf(ctx, ctx->metapos, i);

	while (i < metalen) {
		size_t len = xread(ctx->fd, &meta[i], metalen - i);
		if (len == 0) {
			ctx->eof = true;
			return -1;
		}
		i += len;
	}
	meta[metalen] = 0;

	info("HTTP stream metadata: %s\n", &meta[1]);

	/* TODO: better parser */

	const char title[] = "StreamTitle='";
	if (!strncasecmp(&meta[1], title, strlen(title))) {
		char *text = &meta[1 + strlen(title)];
		char *end = strchr(text, '\'');
		if (end) {
			*end = 0;
			set_streaming_title(get_input_song(ctx->state), text);
		}
	}

	ctx->metapos += ctx->metainterval;
	return 0;
}

static size_t mad_fillbuf(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	size_t t, len;
	while (true) {
		/* remove decoded data from the read buffer */
		if (ctx->stream.next_frame) {
			size_t len = ctx->stream.next_frame - ctx->buffer;
			ctx->fpos += len;
			ctx->metapos -= len;
			skipbuf(ctx, 0, len);
		}

		if (ctx->metainterval && ctx->metapos < ctx->buflen) {
			if (read_meta(ctx))
				return 0;
		}
		mad_stream_buffer(&ctx->stream, ctx->buffer, ctx->buflen);

		if (mad_header_decode(&ctx->frame.header, &ctx->stream)) {
			if (ctx->stream.error == MAD_ERROR_BUFLEN) {
				/* not enought data the in read buffer */
				if (ctx->eof) {
					set_song_length(get_input_song(ctx->state),
						japlay_get_position(ctx->state),
						ctx->reliable ? 100 : 10);
					return 0;
				}
				if (fillbuf(ctx))
					return 0;
			} else
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
			warning("Too small buffer!\n");
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

		t = japlay_get_position(ctx->state) / 1000;
		if (!recall(ctx, t))
			remember(ctx, ctx->fpos, t);
		return len;
	}
}

static int mad_seek(struct input_plugin_ctx *ctx, struct songpos *newpos)
{
	size_t offs;
	size_t curt;
	size_t t = newpos->msecs / 1000;

	if (ctx->length == (size_t) -1)
		return -1;

	curt = japlay_get_position(ctx->state) / 1000;
	if (t == curt)
		return 1;

	offs = recall(ctx, t);
	if (!offs)
		offs = estimate(ctx, t);

	if (ctx->streaming) {
		close(ctx->fd);
		if (connect_http(ctx, offs))
			return -1;
	} else
		lseek(ctx->fd, offs, SEEK_SET);
	ctx->fpos = offs;

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


struct input_plugin *get_input_plugin()
{
	return &plugin_info;
}
