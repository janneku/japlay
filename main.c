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

#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ao/ao.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>

#define SOCKET_NAME		"/tmp/japlay"

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
	unsigned int position; /* position in milliseconds */
	unsigned int pos_cnt;
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
	ds->position = 0;
	ds->pos_cnt = 0;
	ds->format.rate = 0;
	ds->format.channels = 0;
	return 0;
}

static void finish_decode(struct decode_state *ds)
{
	ds->plugin->close(ds->ctx);
	free(ds->ctx);
	put_song(ds->song);
}

static void *playback_thread_routine(void *arg)
{
	UNUSED(arg);

	static sample_t buffer[8192];

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

		int skipsong = 0;
		if (toseek) {
			struct songpos newpos = {.msecs = ds.position};
			long msecs = newpos.msecs + toseek;
			if (msecs < 0)
				msecs = 0;
			newpos.msecs = msecs;
			toseek = 0;
			int seekret = ds.plugin->seek(ds.ctx, &newpos);
			if (seekret < 0) {
				error("Seek error\n");
				skipsong = 1;
			} else if (seekret == 0) {
				warning("Seek not supported\n");
			} else {
				info("Seeking to %ld.%.1lds\n", newpos.msecs / 1000, (newpos.msecs % 1000) / 100);
				ds.position = newpos.msecs;
				ds.pos_cnt = 0;
			}
		}

		size_t len = 0;
		if (!skipsong)
			len = ds.plugin->fillbuf(ds.ctx, buffer, sizeof(buffer) / sizeof(sample_t), &ds.format);
		if (!len) {
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

		if (ds.format.rate != (unsigned int) format.rate ||
		    ds.format.channels != (unsigned int) format.channels || !dev) {
			/* format changed or device is not open */
			if (dev)
				ao_close(dev);
			info("format change: %u Hz, %u channels\n",
				ds.format.rate, ds.format.channels);
			format.rate = ds.format.rate;
			format.channels = ds.format.channels;
			power_cnt = 0;
			power = 0;
			ds.pos_cnt = 0;
			dev = ao_open_live(ao_default_driver_id(), &format, NULL);
			if (!dev) {
				ui_show_message("Unable to open audio device");
				playing = false;
				continue;
			}
		}

		size_t i;
		for (i = power_cnt & 31; i < len; i += 32)
			power += abs(buffer[i]) / 256;

		unsigned int samplerate = ds.format.rate * ds.format.channels;
		ds.pos_cnt += len;

		/* advance song position with full milliseconds from pos_cnt */
		unsigned int adv = ds.pos_cnt * 1000 / samplerate;
		ds.position += adv;
		ds.pos_cnt -= adv * samplerate / 1000;

		/* Update UI status 16 times per second */
		power_cnt += len;
		if (power_cnt >= (unsigned int) samplerate / 16) {
			ui_set_status(power * 64 / power_cnt, ds.position);
			power_cnt = 0;
			power = 0;
		}

		ao_play(dev, (char *)buffer, len * 2);
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
