/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#include "japlay.h"
#include "common.h"
#include "utils.h"
#include "playlist.h"
#include "plugin.h"
#include "ui.h"
#include "iowatch.h"
#include "unixsocket.h"
#include "list.h"
#include "config.h"
#include "buffer.h"

#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ao/ao.h>
#include <limits.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>

#define SOCKET_NAME		"/tmp/japlay"

#define REFRESH_RATE	16   /* how often to run the playback loop */

#define TARGET_POWER	64   /* target power for auto adjustment */

int debug = 0;

static pthread_mutex_t cursor_mutex;

static pthread_mutex_t play_mutex;
static pthread_cond_t play_cond;
static bool playing = false; /* true if we are cursor */

static bool reset = false;
static bool quit = false;

static struct list_head plugins;
static struct song *cursor = NULL;

static pthread_t playback_thread;

static bool autovol = false;
static int volume = 256;

static long toseek = 0;

/* Protects "cursor" */
#define CURSOR_LOCK pthread_mutex_lock(&cursor_mutex)
#define CURSOR_UNLOCK pthread_mutex_unlock(&cursor_mutex)

/* Protects "playing" variable and play_cond */
#define PLAY_LOCK pthread_mutex_lock(&play_mutex)
#define PLAY_UNLOCK pthread_mutex_unlock(&play_mutex)

struct plugin {
	struct list_head head;
	struct input_plugin *info;
};

struct decode_state {
	struct song *song;
	struct input_plugin *plugin;
	struct input_plugin_ctx *ctx;
	struct input_format format; /* current audio format */
	unsigned int position; /* decode position in milliseconds */
	unsigned int pos_cnt;
	unsigned int playposition; /* playback position in milliseconds */
	unsigned int playpos_cnt;
	bool eof;
	struct audio_buffer buffer;
};

static struct decode_state ds;

struct song *get_cursor(void)
{
	CURSOR_LOCK;
	struct song *song = cursor;
	if (song)
		get_song(song);
	CURSOR_UNLOCK;
	return song;
}

void japlay_set_song_length(unsigned int length, bool reliable)
{
	set_song_length(ds.song, length, reliable);
}

unsigned int japlay_get_position(void)
{
	return ds.position;
}

static struct input_plugin *detect_plugin(const char *filename)
{
	struct list_head *pos;
	list_for_each(pos, &plugins) {
		struct plugin *plugin = container_of(pos, struct plugin, head);
		if (plugin->info->detect(filename))
			return plugin->info;
	}
	warning("no plugin for file %s\n", filename);
	return NULL;
}

static void set_cursor_locked(struct song *song)
{
	get_song(song);
	ui_set_cursor(cursor, song);
	if (cursor)
		put_song(cursor);
	cursor = song;
	reset = true;
}

static void set_cursor(struct song *song)
{
	CURSOR_LOCK;
	set_cursor_locked(song);
	CURSOR_UNLOCK;
}

static int init_decode(struct decode_state *ds, struct song *song)
{
	memset(ds, 0, sizeof(*ds));

	const char *filename = get_song_filename(song);

	ds->plugin = detect_plugin(filename);
	if (ds->plugin == NULL)
		return -1;

	ds->ctx = calloc(1, ds->plugin->ctx_size);
	if (ds->ctx == NULL)
		return -1;

	if (ds->plugin->open(ds->ctx, filename)) {
		free(ds->ctx);
		return -1;
	}
	get_song(song);
	ds->song = song;
	init_buffer(&ds->buffer);
	return 0;
}

static void finish_decode(struct decode_state *ds)
{
	ds->plugin->close(ds->ctx);
	free(ds->ctx);
	put_song(ds->song);
}

static void run_decode(struct decode_state *ds)
{
	/* decode as much as possible */
	while (true) {
		size_t max = buffer_write_avail(&ds->buffer, MIN_FILL);
		if (max < MIN_FILL)
			break;

		struct input_format format;
		size_t filled = ds->plugin->fillbuf(ds->ctx,
			write_buffer(&ds->buffer), max, &format);
		if (!filled) {
			ds->eof = true;
			break;
		}

		/* check for format changes */
		if (ds->format.rate != format.rate ||
		    ds->format.channels != format.channels) {
			info("Detected format change\n");
			ds->pos_cnt = 0;
			ds->format = format;
			mark_buffer_formatchg(&ds->buffer);
		}

		buffer_written(&ds->buffer, filled);

		ds->pos_cnt += filled;

		/* advance song position with full milliseconds from pos_cnt */
		unsigned int samplerate = ds->format.rate * ds->format.channels;
		unsigned int adv = ds->pos_cnt * 1000 / samplerate;
		ds->position += adv;
		ds->pos_cnt -= adv * samplerate / 1000;
	}
}

static void *playback_thread_routine(void *arg)
{
	UNUSED(arg);

	ao_device *dev = NULL;
	ao_sample_format format = {.bits = 16, .byte_format = AO_FMT_NATIVE,
			.rate = 0, .channels = 0};
	unsigned int power_cnt = 0, power = 0;

	ds.song = NULL;

	while (!quit) {
		if (reset) {
			/* close the current song file */
			reset = false;
			if (ds.song) {
				finish_decode(&ds);
				ds.song = NULL;
			}
		}

		PLAY_LOCK;
		if (!playing) {
			/* we are not currently playing, sleep */
			pthread_cond_wait(&play_cond, &play_mutex);
			PLAY_UNLOCK;
			continue;
		}
		PLAY_UNLOCK;

		CURSOR_LOCK;
		if (ds.song == NULL) {
			/* start a new song */
			struct song *song = cursor;
			get_song(song);
			CURSOR_UNLOCK;

			if (init_decode(&ds, song)) {
				char *msg = concat_strings("No plugin for file ",
					get_song_filename(song));
				if (msg) {
					ui_show_message(msg);
					free(msg);
				}
				put_song(song);
				playing = false;
				continue;
			}
			put_song(song);
		}
		else
			CURSOR_UNLOCK;

		if (toseek) {
			struct songpos newpos = {.msecs = ds.playposition};
			long msecs = newpos.msecs + toseek;
			if (msecs < 0)
				msecs = 0;
			newpos.msecs = msecs;
			toseek = 0;
			int seekret = ds.plugin->seek(ds.ctx, &newpos);
			if (seekret < 0) {
				error("Seek error\n");
				ds.eof = true;
			} else if (seekret == 0) {
				warning("Seek not supported\n");
			} else {
				info("Seeking to %ld.%.1lds\n", newpos.msecs / 1000, (newpos.msecs % 1000) / 100);
				ds.position = newpos.msecs;
				ds.playposition = newpos.msecs;
				ds.pos_cnt = 0;
				ds.playpos_cnt = 0;
			}
			init_buffer(&ds.buffer);
		}

		if (!ds.eof)
			run_decode(&ds);

		size_t avail = buffer_read_avail(&ds.buffer);

		if (!avail) {
			if (!ds.eof)
				continue;
			reset = true;

			/* if we are still playing the same song, go to next one */
			CURSOR_LOCK;
			if (ds.song == cursor) {
				struct song *next = playlist_next(ds.song, true);
				if (next) {
					set_cursor_locked(next);
					put_song(next);
				} else {
					/* end of playlist, stop */
					playing = false;
				}
			}
			CURSOR_UNLOCK;
			continue;
		}

		bool formatchg = check_buffer_formatchg(&ds.buffer) &&
				(ds.format.rate != (unsigned int) format.rate ||
				 ds.format.channels != (unsigned int) format.channels);
		if (formatchg || !dev) {
			/* format changed or device is not open */
			if (dev)
				ao_close(dev);
			info("format change: %u Hz, %u channels\n",
				ds.format.rate, ds.format.channels);
			format.rate = ds.format.rate;
			format.channels = ds.format.channels;
			power_cnt = 0;
			power = 0;
			ds.playpos_cnt = 0;
			dev = ao_open_live(ao_default_driver_id(), &format, NULL);
			if (!dev) {
				ui_show_message("Unable to open audio device");
				playing = false;
				continue;
			}
		}

		sample_t *buffer = read_buffer(&ds.buffer);

		unsigned int samplerate = ds.format.rate * ds.format.channels;
		if (avail > samplerate / REFRESH_RATE)
			avail = samplerate / REFRESH_RATE;

		size_t i;
		if (volume != 256) {
			for (i = 0; i < avail; ++i) {
				int newsample = buffer[i] * volume / 256;
				if (newsample < SHRT_MIN)
					newsample = SHRT_MIN;
				if (newsample > SHRT_MAX)
					newsample = SHRT_MAX;
				buffer[i] = newsample;
			}
		}

		for (i = power_cnt & 31; i < avail; i += 32)
			power += abs(buffer[i]) / 256;

		ds.playpos_cnt += avail;

		/* advance song position with full milliseconds from pos_cnt */
		unsigned int adv = ds.playpos_cnt * 1000 / samplerate;
		ds.playposition += adv;
		ds.playpos_cnt -= adv * samplerate / 1000;

		/* Update UI status */
		power_cnt += avail;
		if (power_cnt >= samplerate / REFRESH_RATE) {
			/* Scale power to 0..255 */
			power = power * 64 / power_cnt;

			/* Automatic volume adjust */
			int zone = TARGET_POWER / 3;
			if (autovol && (power < TARGET_POWER-zone || power > TARGET_POWER+zone)) {
				volume += (TARGET_POWER - (int)power) / zone;
				info("autovol %d%%\n", volume * 100 / 256);
			}

			ui_set_status(power, ds.playposition);
			power_cnt = 0;
			power = 0;
		}

		ao_play(dev, (char *)buffer, avail * 2);

		buffer_processed(&ds.buffer, avail);
	}

	return NULL;
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

struct song *add_file_playlist(const char *filename)
{
	char *path = absolute_path(filename);
	if (!path)
		return NULL;
	struct song *song = new_song(path);
	if (song)
		add_playlist(song);
	free(path);
	return song;
}

bool load_playlist_pls(const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return false;

	char row[512];
	while (fgets(row, sizeof(row), f)) {
		char *value = strchr(row, '=');
		if (value == NULL)
			continue;
		*value = 0;
		value++;
		if (!memcmp(trim(row), "File", 4)) {
			char *fname = build_filename(filename, trim(value));
			if (fname) {
				struct song *song = add_file_playlist(fname);
				if (song)
					put_song(song);
				free(fname);
			}
		}
	}
	fclose(f);
	return true;
}

bool load_playlist_m3u(const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return false;

	char row[512];
	struct song *song = NULL;
	while (fgets(row, sizeof(row), f)) {
		if (row[0] != '#') {
			if (song)
				put_song(song);
			song = NULL;
			char *fname = build_filename(filename, trim(row));
			if (fname) {
				song = add_file_playlist(fname);
				free(fname);
			}
		} else if(!memcmp(row, "#length ", 8) && song) {
			set_song_length(song, atoi(&row[8]), false);
		}
	}
	if (song)
		put_song(song);
	fclose(f);
	return true;
}

static void kick_playback(void)
{
	PLAY_LOCK;
	playing = true;
	pthread_cond_signal(&play_cond);
	PLAY_UNLOCK;
}

void play_playlist(struct song *song)
{
	set_cursor(song);
	kick_playback();
}

void japlay_play(void)
{
	CURSOR_LOCK;
	if (cursor == NULL) {
		struct song *song = get_playlist_first();
		if (song == NULL) {
			CURSOR_UNLOCK;
			return;
		}
		set_cursor_locked(song);
		put_song(song);
	}
	CURSOR_UNLOCK;
	kick_playback();
}

void japlay_set_autovol(bool enabled)
{
	autovol = enabled;
}

void japlay_seek_relative(long msecs)
{
	toseek = msecs;
}

void japlay_stop(void)
{
	reset = true;
	playing = false;
}

void japlay_pause(void)
{
	playing = false;
}

void japlay_skip(void)
{
	struct song *song = get_cursor();
	struct song *next = playlist_next(song, true);
	put_song(song);
	if (next) {
		set_cursor(next);
		put_song(next);
	}
}

static int dummy_seek(struct input_plugin_ctx *ctx,
		      struct songpos *newpos)
{
	UNUSED(ctx);
	UNUSED(newpos);
	return 0;
}

static bool load_plugin(const char *filename)
{
	void *dl = dlopen(filename, RTLD_NOW);
	if (dl == NULL)
		return false;

	struct input_plugin *(*get_info)(void) = dlsym(dl, "get_info");
	if (get_info == NULL) {
		warning("Not a japlay plugin: %s\n", file_base(filename));
		goto err;
	}

	struct input_plugin *info = get_info();

	if (info->seek == NULL)
		info->seek = dummy_seek;

	info("found plugin: %s (%s)\n", file_base(filename), info->name);

	struct plugin *plugin = NEW(struct plugin);
	if (plugin == NULL)
		goto err;
	plugin->info = info;
	list_add_tail(&plugin->head, &plugins);
	return true;

 err:
	dlclose(dl);
	return false;
}

static void load_plugins(void)
{
	list_init(&plugins);

	DIR *dir = opendir(PLUGIN_DIR);
	if (!dir)
		return;

	static const char plugin_dir[] = PLUGIN_DIR "/";

	struct dirent *de = readdir(dir);
	while (de) {
		if (de->d_name[0] != '.') {
			char *buf = concat_strings(plugin_dir, de->d_name);
			if (buf) {
				if (!load_plugin(buf))
					warning("Unable to load plugin %s\n", de->d_name);
				free(buf);
			}
		}
		de = readdir(dir);
	}

	closedir(dir);
}

int japlay_connect(void)
{
	return unix_socket_connect(SOCKET_NAME);
}

void japlay_send(int fd, const char *filename)
{
	char *path = absolute_path(filename);
	if (path) {
		sendto(fd, path, strlen(path), 0, NULL, 0);
		free(path);
	}
}

static int incoming_data(int fd, int flags, void *ctx)
{
	UNUSED(flags);
	UNUSED(ctx);

	char filename[PATH_MAX + 1];
	ssize_t len = recvfrom(fd, filename, PATH_MAX, 0, NULL, 0);
	if (len < 0) {
		if (errno == EAGAIN)
			return 0;
		warning("recv failed (%s)\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (len == 0) {
		close(fd);
		return -1;
	}
	filename[len] = 0;
	struct song *song = add_file_playlist(filename);
	if (song)
		put_song(song);
	return 0;
}

static int incoming_client(int fd, int flags, void *ctx)
{
	UNUSED(flags);
	UNUSED(ctx);

	struct sockaddr_un addr;
	socklen_t addrlen = sizeof(addr);
	int rfd = accept(fd, (struct sockaddr *)&addr, &addrlen);
	if (rfd < 0)
		warning("accept failure (%s)\n", strerror(errno));
	else
		new_io_watch(rfd, IO_IN, incoming_data, NULL);
	return 0;
}

int japlay_init(int *argc, char **argv)
{
	int i, newargc = 1;
	for (i = 1; i < *argc; ++i) {
		if (!strcmp(argv[i], "-d")) {
			debug = 1;
		} else
			argv[newargc++] = argv[i];
	}
	*argc = newargc;

	const char *configdir = get_config_dir();
	if (configdir == NULL) {
		error("Can not determine/allocate config dir: $HOME/.japlay");
		return -1;
	}
	if (mkdir(configdir, 0700) && errno != EEXIST) {
		error("Can not create config dir: %s (%s)\n", configdir, strerror(errno));
		return -1;
	}

	ao_initialize();
	load_plugins();

	int count;
	ao_info **drivers = ao_driver_info_list(&count);
	for (i = 0; i < count; ++i) {
		if (drivers[i]->type == AO_TYPE_LIVE) {
			info("ao driver: %s (%s)\n", drivers[i]->short_name,
				drivers[i]->name);
		}
	}

	init_playlist();
	iowatch_init();

	pthread_mutex_init(&cursor_mutex, NULL);

	pthread_mutex_init(&play_mutex, NULL);
	pthread_cond_init(&play_cond, NULL);

	pthread_create(&playback_thread, NULL, playback_thread_routine, NULL);

	int fd = unix_socket_create(SOCKET_NAME);
	if (fd >= 0)
		new_io_watch(fd, IO_IN, incoming_client, NULL);

	return 0;
}

void japlay_exit(void)
{
	quit = true;
	void *retval;
	kick_playback();
	pthread_join(playback_thread, &retval);

	unlink(SOCKET_NAME);
}
