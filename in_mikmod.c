#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <mikmod.h>
#include <glib.h>
#include <glib/gprintf.h>

typedef struct mikmod_context *plugin_ctx_t;
#include "plugin.h"

struct mikmod_context {
	MODULE *mf;
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

static bool dummy_IsThere()
{
	return true;
}

static void dummy_Update()
{
	//length = VC_WriteBytes((SBYTE *) audiobuffer, buffer_size);
}

static bool dummy_Reset(void)
{
	VC_Exit();
	return VC_Init();
}

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
	dummy_Reset,
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

static struct mikmod_context *mikmod_open(const char *filename)
{
	struct mikmod_context *ctx = g_new(struct mikmod_context, 1);
	if (!ctx)
		return NULL;

	MikMod_RegisterAllLoaders();
	MikMod_RegisterDriver(&drv_dummy);

	md_mode |= DMODE_16BITS | DMODE_INTERP | DMODE_STEREO;
	md_mixfreq = 44100;

	MikMod_Init("");

	ctx->mf = Player_Load((char *)filename, 128, true);
	if (!ctx->mf) {
		g_printf("MikMod error: %s\n", MikMod_strerror(MikMod_errno));
		g_free(ctx);
		return NULL;
	}

	Player_Start(ctx->mf);

	return ctx;
}

static void mikmod_close(struct mikmod_context *ctx)
{
	Player_Free(ctx->mf);
	g_free(ctx);
}

static size_t mikmod_fillbuf(struct mikmod_context *ctx, sample_t *buffer,
			  size_t maxlen, struct input_format *format)
{
	if (!Player_Active())
		return 0;

	format->channels = 2;
	format->rate = 44100;

	return VC_WriteBytes((SBYTE *)buffer, maxlen * sizeof(sample_t))
		/ sizeof(sample_t);
}

static const struct input_plugin plugin_info = {
	sizeof(struct input_plugin),
	"MikMod module player",
	mikmod_detect,
	mikmod_open,
	mikmod_close,
	mikmod_fillbuf,
};

const struct input_plugin *get_info()
{
	return &plugin_info;
}