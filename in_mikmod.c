/*
 * japlay MikMod module player plugin
 * Copyright Janne Kulmala 2010
 */
#include "common.h"
#include "playlist.h"
#include "utils.h"
#include "plugin.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mikmod.h>

struct input_plugin_ctx {
	struct input_state *state;
	MODULE *mf;
	MREADER mr;
	int fd;
	bool eof;
};

static bool mikmod_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	if (!ext)
		return false;
	return !strcasecmp(ext, "mod") ||
		!strcasecmp(ext, "s3m") ||
		!strcasecmp(ext, "xm") ||
		!strcasecmp(ext, "it");
}

static BOOL dummy_IsThere()
{
	return true;
}

static void dummy_Update()
{
	//length = VC_WriteBytes((SBYTE *) audiobuffer, buffer_size);
}

static BOOL reader_Seek(struct MREADER *mr, long off, int whence)
{
	struct input_plugin_ctx *ctx =
		container_of(mr, struct input_plugin_ctx, mr);
	lseek(ctx->fd, off, whence);
	return true;
}

static long reader_Tell(struct MREADER *mr)
{
	struct input_plugin_ctx *ctx =
		container_of(mr, struct input_plugin_ctx, mr);
	return lseek(ctx->fd, 0, SEEK_CUR);
}

static BOOL reader_Read(struct MREADER *mr, void *buf, size_t maxlen)
{
	struct input_plugin_ctx *ctx =
		container_of(mr, struct input_plugin_ctx, mr);

	ssize_t len = read_in_full(ctx->fd, buf, maxlen);
	if (len < 0)
		linewarning("read failed (%s)\n", strerror(errno));
	if ((size_t) len != maxlen) {
		ctx->eof = true;
		return false;
	}
	return true;
}

static int reader_Get(struct MREADER *mr)
{
	struct input_plugin_ctx *ctx =
		container_of(mr, struct input_plugin_ctx, mr);
	unsigned char c;
	if (read(ctx->fd, &c, 1) < 1) {
		ctx->eof = true;
		return EOF;
	}
	return c;
}

static BOOL reader_Eof(struct MREADER *mr)
{
	struct input_plugin_ctx *ctx =
		container_of(mr, struct input_plugin_ctx, mr);
	return ctx->eof;
}

static MREADER my_reader = {
	reader_Seek,
	reader_Tell,
	reader_Read,
	reader_Get,
	reader_Eof
};

static MDRIVER drv_dummy = {
	NULL,
	"dummy",
	"dummy output driver for in_mikmod",
	0, 255,
	"dummy",
	NULL,
#if (LIBMIKMOD_VERSION >= 0x030200)
	NULL,
#endif
	dummy_IsThere,
	VC_SampleLoad,
	VC_SampleUnload,
	VC_SampleSpace,
	VC_SampleLength,
	VC_Init,
	VC_Exit,
	NULL,
	VC_SetNumVoices,
	VC_PlayStart,
	VC_PlayStop,
	dummy_Update,
	NULL,
	VC_VoiceSetVolume,
	VC_VoiceGetVolume,
	VC_VoiceSetFrequency,
	VC_VoiceGetFrequency,
	VC_VoiceSetPanning,
	VC_VoiceGetPanning,
	VC_VoicePlay,
	VC_VoiceStop,
	VC_VoiceStopped,
	VC_VoiceGetPosition,
	VC_VoiceRealVolume
};

static void init_mikmod(void)
{
	/* TODO: this is racy */
	static bool mikmod_init = false;
	if (!mikmod_init) {
		mikmod_init = true;
		MikMod_RegisterAllLoaders();
		MikMod_RegisterDriver(&drv_dummy);

		md_mode |= DMODE_16BITS | DMODE_INTERP | DMODE_STEREO;
		md_mixfreq = 44100;

		MikMod_Init("");
	}
}

static int mikmod_scan(struct song *song)
{
	init_mikmod();

	MODULE *mf = Player_Load((char *)get_song_filename(song), 128, true);
	if (mf == NULL) {
		warning("MikMod error: %s\n", MikMod_strerror(MikMod_errno));
		return -1;
	}
	set_song_title(song, mf->songname);
	Player_Free(mf);
	return 0;
}

static int mikmod_open(struct input_plugin_ctx *ctx, struct input_state *state,
		       const char *filename)
{
	struct song *song = get_input_song(state);
	ctx->state = state;

	init_mikmod();

	ctx->fd = open(filename, O_RDONLY);
	if (ctx->fd < 0) {
		warning("unable to open file (%s)\n", strerror(errno));
		return -1;
	}

	ctx->mr = my_reader;

	ctx->mf = Player_LoadGeneric(&ctx->mr, 128, true);
	if (ctx->mf == NULL) {
		warning("MikMod error: %s\n", MikMod_strerror(MikMod_errno));
		close(ctx->fd);
		return -1;
	}

	set_song_title(song, ctx->mf->songname);

	Player_Start(ctx->mf);

	return 0;
}

static void mikmod_close(struct input_plugin_ctx *ctx)
{
	Player_Stop();
	Player_Free(ctx->mf);
	close(ctx->fd);
}

static size_t mikmod_fillbuf(struct input_plugin_ctx *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	UNUSED(ctx);

	if (!Player_Active()) {
		set_song_length(get_input_song(ctx->state),
				japlay_get_position(ctx->state), 100);
		return 0;
	}

	format->channels = 2;
	format->rate = 44100;

	return VC_WriteBytes((SBYTE *)buffer, maxlen * sizeof(sample_t))
		/ sizeof(sample_t);
}

static const char *mime_types[] = {
	"audio/x-mod",
	"audio/x-s3m",
	NULL
};

static struct input_plugin plugin_info = {
	.size = sizeof(struct input_plugin),
	.ctx_size = sizeof(struct input_plugin_ctx),
	.name = "MikMod module player",
	.detect = mikmod_detect,
	.scan = mikmod_scan,
	.open = mikmod_open,
	.close = mikmod_close,
	.fillbuf = mikmod_fillbuf,
	.mime_types = mime_types,
};

struct input_plugin *get_input_plugin()
{
	return &plugin_info;
}
