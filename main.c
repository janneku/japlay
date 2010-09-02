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
#include "list.h"
#include "config.h"
#include "buffer.h"

#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ao/ao.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>

#define SOCKET_NAME		"/tmp/japlay"

#define REFRESH_RATE	16   /* how often to run the playback loop */

#define TARGET_POWER	64   /* target power for auto adjustment */

int japlay_debug = 0;

static pthread_mutex_t cursor_mutex;

/* decode thread */
static pthread_t decode_thread;
static pthread_mutex_t play_mutex;
static pthread_cond_t decode_cond;
static bool playing = false; /* true if we are playing a song */

/* play thread */
static pthread_cond_t play_cond;
static pthread_t play_thread;
static struct audio_buffer play_buffer;

static pthread_t scan_thread;
static pthread_mutex_t scan_mutex;
static pthread_cond_t scan_cond;
static bool scanning = false; /* true if we are scanning the playlist */

static bool reset = false;
static bool quit = false;

static struct list_head input_plugins;
static struct list_head playlist_plugins;

static struct playlist_entry *cursor = NULL;

static bool autovol = false;
static int volume = 256;

static unsigned int toseek = -1;

struct playlist *japlay_queue, *japlay_history;

/* Protects "cursor" */
#define CURSOR_LOCK pthread_mutex_lock(&cursor_mutex)
#define CURSOR_UNLOCK pthread_mutex_unlock(&cursor_mutex)

/* Protects "playing" variable, decode_cond, play_cond and play_buffer */
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
	/* decode position in milliseconds */
	unsigned int position;
	unsigned int pos_cnt;
	/* playback position in milliseconds */
	unsigned int playposition;
	unsigned int playpos_cnt;
	unsigned int toseek;
};

static struct input_state ds;

void set_streaming_title(struct song *song, const char *title)
{
	CURSOR_LOCK;
	if (song == get_entry_song(cursor))
		ui_set_streaming_title(title);
	CURSOR_UNLOCK;
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

static void advance_queue_locked(void)
{
	/* remove from the queue */
	if (cursor)
		remove_playlist(japlay_queue, cursor);

	/* add to the song history */
	if (cursor) {
		struct playlist_entry *entry =
			add_playlist(japlay_history, get_entry_song(cursor),
				     false);
		if (entry)
			put_entry(entry);
		put_entry(cursor);
	}
	cursor = get_playlist_first(japlay_queue);
	ui_set_cursor(cursor);
	reset = true;
}

static void advance_queue(void)
{
	CURSOR_LOCK;
	advance_queue_locked();
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
	ds->toseek = -1;

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
	return 0;
}

static void finish_input(struct input_state *ds)
{
	ds->plugin->close(ds->ctx);
	free(ds->ctx);
	put_song(ds->song);
}

static void *decode_thread_routine(void *arg)
{
	UNUSED(arg);

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

		size_t avail;

		PLAY_LOCK;
		if (!playing) {
			/* we are not currently playing, sleep */
			pthread_cond_wait(&decode_cond, &play_mutex);
			PLAY_UNLOCK;
			continue;
		} else {
			avail = buffer_write_avail(&play_buffer, MIN_FILL);
			if (avail < MIN_FILL) {
				/* buffer is full, wake up play thread and sleep */
				pthread_cond_signal(&play_cond);
				pthread_cond_wait(&decode_cond, &play_mutex);
				PLAY_UNLOCK;
				continue;
			}
		}
		PLAY_UNLOCK;

		/* avail >= MIN_FILL and playing == true */

		CURSOR_LOCK;
		if (ds.song == NULL) {
			/* start a new song */
			struct song *song = NULL;
			if (cursor) {
				song = get_entry_song(cursor);
				get_song(song);
			}
			CURSOR_UNLOCK;
			if (song == NULL) {
				playing = false;
				continue;
			}

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

		if (toseek != (unsigned int) -1) {
			struct songpos newpos = {.msecs = toseek,};
			toseek = -1;
			int seekret = ds.plugin->seek(ds.ctx, &newpos);
			if (seekret < 0) {
				error("Seek error\n");
				goto diediedie;
			} else if (seekret == 0) {
				warning("Seek not supported\n");
			} else {
				info("Seeking to %ld.%.1lds\n", newpos.msecs / 1000, (newpos.msecs % 1000) / 100);
				ds.position = newpos.msecs;
				ds.pos_cnt = 0;
				ds.toseek = newpos.msecs;
				mark_buffer_event(&play_buffer);
			}
			/* FIXME: clear audio buffer
			PLAY_LOCK;
			init_buffer(&play_buffer);
			PLAY_UNLOCK;*/
		}

		struct input_format format;
		size_t filled = ds.plugin->fillbuf(ds.ctx,
			write_buffer(&play_buffer), avail, &format);
		if (!filled) {
		diediedie:
			advance_queue();
			continue;
		}

		/* check for format changes */
		if (ds.format.rate != format.rate ||
		    ds.format.channels != format.channels) {
			info("Detected format change\n");
			ds.pos_cnt = 0;
			ds.format = format;
			mark_buffer_event(&play_buffer);
		}

		PLAY_LOCK;
		buffer_written(&play_buffer, filled);
		PLAY_UNLOCK;

		ds.pos_cnt += filled;

		/* advance song position with full milliseconds from pos_cnt */
		unsigned int samplerate = ds.format.rate * ds.format.channels;
		unsigned int adv = ds.pos_cnt * 1000 / samplerate;
		ds.position += adv;
		ds.pos_cnt -= adv * samplerate / 1000;
	}

	if (ds.song)
		finish_input(&ds);
	return NULL;
}

static void *play_thread_routine(void *arg)
{
	UNUSED(arg);

	ao_device *dev = NULL;
	ao_sample_format format = {.bits = 16, .byte_format = AO_FMT_NATIVE,
			.rate = 0, .channels = 0};
	unsigned int power_cnt = 0, power = 0;
	int scope[SCOPE_SIZE];

	while (!quit) {
		size_t avail;

		PLAY_LOCK;
		avail = buffer_read_avail(&play_buffer);
		if (avail == 0 && !check_buffer_event(&play_buffer)) {
			/* buffer is empty, sleep */
			pthread_cond_wait(&play_cond, &play_mutex);
			PLAY_UNLOCK;
			continue;
		}
		PLAY_UNLOCK;

		if (ds.toseek != (unsigned int) -1) {
			info("playback seek\n");
			ds.playposition = ds.toseek;
			ds.toseek = -1;
			ds.playpos_cnt = 0;
		}

		bool formatchg =
			ds.format.rate != (unsigned int) format.rate ||
			ds.format.channels != (unsigned int) format.channels;
		if (formatchg || !dev) {
			/* format changed detected or device is not open */
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
			if (dev == NULL) {
				ui_show_message("Unable to open audio device");
				playing = false;
				continue;
			}
		}

		sample_t *buffer = read_buffer(&play_buffer);

		unsigned int samplerate = format.rate * format.channels;
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

		for (i = 31 - (power_cnt & 31); i < avail; i += 32) {
			power += abs(buffer[i]) / 256;

			int j = (power_cnt + i) / 32;
			if (j < SCOPE_SIZE)
				scope[j] = buffer[i];
		}
		power_cnt += avail;

		ds.playpos_cnt += avail;

		/* advance song position with full milliseconds from pos_cnt */
		unsigned int adv = ds.playpos_cnt * 1000 / samplerate;
		ds.playposition += adv;
		ds.playpos_cnt -= adv * samplerate / 1000;

		/* Update UI status */
		if (power_cnt >= samplerate / REFRESH_RATE) {
			/* Scale power to 0..255 */
			power = power * 64 / power_cnt;

			/* Automatic volume adjust */
			unsigned int zone = TARGET_POWER / 3;
			if (autovol && (power < TARGET_POWER-zone || power > TARGET_POWER+zone)) {
				volume += (TARGET_POWER - (int)power) / zone;
				info("autovol %d%%\n", volume * 100 / 256);
			}

			ui_set_status(scope, power_cnt / 32, ds.playposition);
			power_cnt = 0;
			power = 0;
		}

		ao_play(dev, (char *)buffer, avail * 2);

		/* we are done with the audio data */
		PLAY_LOCK;
		buffer_processed(&play_buffer, avail);
		pthread_cond_signal(&decode_cond);
		PLAY_UNLOCK;
	}

	if (dev)
		ao_close(dev);
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

		/* FIXME: implement scan queue */
		scan_playlist(japlay_queue);
		scanning = false;
	}

	return NULL;
}

struct playlist_entry *add_file_playlist(struct playlist *playlist,
					 const char *filename)
{
	char *path = absolute_path(filename);
	if (path == NULL)
		return NULL;
	struct song *song = new_song(path);
	free(path);
	if (song == NULL)
		return NULL;
	struct playlist_entry *entry = add_playlist(playlist, song, false);
	put_song(song);
	return entry;
}

int add_dir_or_file_playlist(struct playlist *playlist, const char *path)
{
	DIR *dir = opendir(path);
	if (dir == NULL) {
		/* check if we can play the file */
		if (detect_input_plugin(path)) {
			struct playlist_entry *entry =
				add_file_playlist(playlist, path);
			if (entry)
				put_entry(entry);
		}
		return 0;
	}

	struct dirent *de = readdir(dir);
	while (de) {
		/* skip parent directory link and hidden files */
		if (de->d_name[0] == '.') {
			de = readdir(dir);
			continue;
		}
		char *fname = concat_path(path, de->d_name);
		if (fname) {
			add_dir_or_file_playlist(playlist, fname);
			free(fname);
		}
		de = readdir(dir);
	}

	closedir(dir);
	return 0;
}

int load_playlist(struct playlist *playlist, const char *filename)
{
	struct playlist_plugin *plugin = detect_playlist_plugin(filename);
	if (plugin == NULL)
		return -1;
	return plugin->load(playlist, filename);
}

static void kick_playback(void)
{
	PLAY_LOCK;
	playing = true;
	pthread_cond_signal(&decode_cond);
	PLAY_UNLOCK;
}

void start_playlist_scan(void)
{
	SCAN_LOCK;
	scanning = true;
	pthread_cond_signal(&scan_cond);
	SCAN_UNLOCK;
}

void japlay_play(void)
{
	CURSOR_LOCK;
	if (cursor == NULL)
		advance_queue_locked();
	CURSOR_UNLOCK;
	kick_playback();
}

void japlay_set_autovol(bool enabled)
{
	autovol = enabled;
}

void japlay_seek_relative(long msecs)
{
	japlay_seek(ds.playposition + msecs);
}

void japlay_seek(long position)
{
	if (position < 0)
		position = 0;
	toseek = position;
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
	advance_queue();
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
	if (dir == NULL)
		return;

	struct dirent *de = readdir(dir);
	while (de) {
		/* skip parent directory link and hidden files */
		if (de->d_name[0] == '.') {
			de = readdir(dir);
			continue;
		}
		char *fname = concat_path(PLUGIN_DIR, de->d_name);
		if (fname) {
			if (!load_plugin(fname))
				warning("Unable to load plugin %s\n", de->d_name);
			free(fname);
		}
		de = readdir(dir);
	}

	closedir(dir);
}

int japlay_init(int *argc, char **argv)
{
	int i, newargc = 1;
	for (i = 1; i < *argc; ++i) {
		if (strcmp(argv[i], "-d") == 0) {
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

	init_buffer(&play_buffer);

	pthread_mutex_init(&cursor_mutex, NULL);

	pthread_mutex_init(&play_mutex, NULL);

	pthread_mutex_init(&scan_mutex, NULL);

	pthread_cond_init(&play_cond, NULL);
	pthread_cond_init(&decode_cond, NULL);
	pthread_cond_init(&scan_cond, NULL);

	pthread_create(&decode_thread, NULL, decode_thread_routine, NULL);
	pthread_create(&play_thread, NULL, play_thread_routine, NULL);
	pthread_create(&scan_thread, NULL, scan_thread_routine, NULL);

	japlay_queue = new_playlist();
	japlay_history = new_playlist();

	return 0;
}

void japlay_exit(void)
{
	quit = true;
	pthread_cond_signal(&decode_cond);
	pthread_cond_signal(&play_cond);
	pthread_cond_signal(&scan_cond);
	void *retval;
	pthread_join(decode_thread, &retval);
	pthread_join(play_thread, &retval);
	pthread_join(scan_thread, &retval);
}
