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

int japlay_debug = 0;

static pthread_mutex_t cursor_mutex;

static pthread_t playback_thread;
static pthread_mutex_t play_mutex;
static pthread_cond_t play_cond;
static bool playing = false; /* true if we are playing a song */

static pthread_t scan_thread;
static pthread_mutex_t scan_mutex;
static pthread_cond_t scan_cond;
static bool scanning = false; /* true if we are scanning the playlist */

static bool reset = false;
static bool quit = false;

static struct list_head input_plugins;
static struct list_head playlist_plugins;

static struct song *cursor = NULL;

static bool autovol = false;
static int volume = 256;

static long toseek = 0;

/* Protects "cursor" */
#define CURSOR_LOCK pthread_mutex_lock(&cursor_mutex)
#define CURSOR_UNLOCK pthread_mutex_unlock(&cursor_mutex)

/* Protects "playing" variable and play_cond */
#define PLAY_LOCK pthread_mutex_lock(&play_mutex)
#define PLAY_UNLOCK pthread_mutex_unlock(&play_mutex)

/* Protects "scanning" variable and scan_cond */
#define SCAN_LOCK pthread_mutex_lock(&scan_mutex)
#define SCAN_UNLOCK pthread_mutex_unlock(&scan_mutex)

struct input_plugin_item {
	struct list_head head;
	struct input_plugin *info;
};

struct playlist_plugin_item {
	struct list_head head;
	struct playlist_plugin *info;
};

struct input_state {
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

static struct input_state ds;

struct song *get_cursor(void)
{
	CURSOR_LOCK;
	struct song *song = cursor;
	if (song)
		get_song(song);
	CURSOR_UNLOCK;
	return song;
}

struct song *get_input_song(struct input_state *state)
{
	return state->song;
}

unsigned int japlay_get_position(struct input_state *state)
{
	return state->position;
}

static struct input_plugin *detect_input_plugin(const char *filename)
{
	struct list_head *pos;
	list_for_each(pos, &input_plugins) {
		struct input_plugin_item *plugin
			= container_of(pos, struct input_plugin_item, head);
		if (plugin->info->detect(filename))
			return plugin->info;
	}
	warning("no plugin for file %s\n", filename);
	return NULL;
}

static struct playlist_plugin *detect_playlist_plugin(const char *filename)
{
	struct list_head *pos;
	list_for_each(pos, &playlist_plugins) {
		struct playlist_plugin_item *plugin
			= container_of(pos, struct playlist_plugin_item, head);
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

int get_song_info(struct song *song)
{
	const char *filename = get_song_filename(song);

	struct input_plugin *plugin = detect_input_plugin(filename);
	if (plugin == NULL)
		return -1;

	return plugin->scan(song);
}

static int init_input(struct input_state *ds, struct song *song)
{
	memset(ds, 0, sizeof(*ds));

	const char *filename = get_song_filename(song);

	ds->plugin = detect_input_plugin(filename);
	if (ds->plugin == NULL)
		return -1;

	ds->ctx = calloc(1, ds->plugin->ctx_size);
	if (ds->ctx == NULL)
		return -1;

	ds->song = song;
	if (ds->plugin->open(ds->ctx, ds, filename)) {
		ds->song = NULL;
		free(ds->ctx);
		return -1;
	}
	get_song(ds->song);
	init_buffer(&ds->buffer);
	return 0;
}

static void finish_input(struct input_state *ds)
{
	ds->plugin->close(ds->ctx);
	free(ds->ctx);
	put_song(ds->song);
}

static void run_input(struct input_state *ds)
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
				finish_input(&ds);
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

			if (init_input(&ds, song)) {
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
			run_input(&ds);

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

static void *scan_thread_routine(void *arg)
{
	UNUSED(arg);

	while (!quit) {
		SCAN_LOCK;
		if (!scanning) {
			/* we are not currently scanning, sleep */
			pthread_cond_wait(&scan_cond, &scan_mutex);
			SCAN_UNLOCK;
			continue;
		}
		SCAN_UNLOCK;

		scan_playlist();
		scanning = false;
	}

	return NULL;
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

int load_playlist(const char *filename)
{
	struct playlist_plugin *plugin = detect_playlist_plugin(filename);
	if (plugin == NULL)
		return -1;
	return plugin->load(filename);
}

static void kick_playback(void)
{
	PLAY_LOCK;
	playing = true;
	pthread_cond_signal(&play_cond);
	PLAY_UNLOCK;
}

void start_playlist_scan(void)
{
	SCAN_LOCK;
	scanning = true;
	pthread_cond_signal(&scan_cond);
	SCAN_UNLOCK;
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

static int dummy_scan(struct song *song)
{
	UNUSED(song);
	return 0;
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

	get_input_plugin_t get_input_plugin
		= (get_input_plugin_t *) dlsym(dl, "get_input_plugin");
	if (get_input_plugin) {
		struct input_plugin *info = get_input_plugin();

		if (info->seek == NULL)
			info->seek = dummy_seek;
		if (info->scan == NULL)
			info->scan = dummy_scan;

		info("found input plugin: %s (%s)\n", file_base(filename), info->name);

		struct input_plugin_item *plugin = NEW(struct input_plugin_item);
		if (plugin != NULL) {
			plugin->info = info;
			list_add_tail(&plugin->head, &input_plugins);
		}
	}

	get_playlist_plugin_t get_playlist_plugin
		= (get_playlist_plugin_t *) dlsym(dl, "get_playlist_plugin");
	if (get_playlist_plugin) {
		struct playlist_plugin *info = get_playlist_plugin();

		info("found playlist plugin: %s (%s)\n", file_base(filename), info->name);

		struct playlist_plugin_item *plugin = NEW(struct playlist_plugin_item);
		if (plugin != NULL) {
			plugin->info = info;
			list_add_tail(&plugin->head, &playlist_plugins);
		}
	}
	return true;
}

static void load_plugins(void)
{
	list_init(&input_plugins);
	list_init(&playlist_plugins);

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

	char filename[FILENAME_MAX + 1];
	ssize_t len = recvfrom(fd, filename, FILENAME_MAX, 0, NULL, 0);
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
			japlay_debug = 1;
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

	pthread_mutex_init(&scan_mutex, NULL);
	pthread_cond_init(&scan_cond, NULL);
	pthread_create(&scan_thread, NULL, scan_thread_routine, NULL);

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
	start_playlist_scan();
	pthread_join(playback_thread, &retval);
	pthread_join(scan_thread, &retval);

	unlink(SOCKET_NAME);
}
