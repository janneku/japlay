/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#include "japlay.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <ao/ao.h>

#define APP_NAME		"japlay"

#define PLUGIN_DIR		"/usr/lib/japlay"

#define UNUSED(x)		(void)x

#define strcpy_q(d, s)		\
	memcpy(d, s, strlen(s) + 1)

struct song {
	struct list_head head;
	atomic_t refcount;
	char *filename;
	struct song_ui_ctx *ui_ctx;
};

static GMutex *playlist_mutex;
static GMutex *playing_mutex;
static GMutex *play_mutex;
static GCond *play_cond;
static bool play = false;
static bool reset = false;
static struct list_head playlist;
static int playlist_len = 0;
static GList *plugins = NULL;
static struct song *playing = NULL;

static char *get_cwd()
{
	size_t len = 64;
	char *buf = NULL;
	while (len <= 4096) {
		buf = realloc(buf, len);
		if (!buf)
			return NULL;
		if (getcwd(buf, len))
			return buf;
		else if (errno != ERANGE)
			break;
		len *= 2;
	}
	free(buf);
	return NULL;
}

static char *absolute_path(const char *filename)
{
	if (!memcmp(filename, "http://", 7) || filename[0] == '/')
		return strdup(filename);

	char *cwd = get_cwd();
	if (!cwd)
		return NULL;

	size_t cwd_len = strlen(cwd);
	cwd = realloc(cwd, cwd_len + strlen(filename) + 2);
	if (!cwd)
		return NULL;
	cwd[cwd_len] = '/';
	strcpy_q(&cwd[cwd_len + 1], filename);
	return cwd;
}

struct song_ui_ctx *get_song_ui_ctx(struct song *song)
{
	return song->ui_ctx;
}

const char *get_song_filename(struct song *song)
{
	return song->filename;
}

struct song *new_song(const char *filename)
{
	char *fname = absolute_path(filename);
	if (!fname)
		return NULL;
	printf("adding %s\n", fname);

	struct song *song = g_new0(struct song, 1);
	if (!song) {
		free(fname);
		return NULL;
	}
	song->filename = fname;
	atomic_set(&song->refcount, 1);
	song->ui_ctx = g_malloc(ui_song_ctx_size);

	return song;
}

void get_song(struct song *song)
{
	atomic_inc(&song->refcount);
}

void put_song(struct song *song)
{
	if (atomic_dec_and_test(&song->refcount)) {
		free(song->filename);
		g_free(song->ui_ctx);
		g_free(song);
	}
}

struct song *get_playing()
{
	g_mutex_lock(playing_mutex);
	struct song *song = playing;
	if (song)
		get_song(song);
	g_mutex_unlock(playing_mutex);
	return song;
}

struct input_plugin *detect_plugin(const char *filename)
{
	GList *iter = plugins;
	while (iter) {
		struct input_plugin *plugin = iter->data;
		if (plugin->detect(filename))
			return plugin;
		iter = iter->next;
	}
	printf("no plugin for file %s\n", filename);
	return NULL;
}

static void set_playing(struct song *song)
{
	get_song(song);

	g_mutex_lock(playing_mutex);
	struct song *prev = playing;
	playing = song;
	reset = true;
	ui_set_playing(prev, playing);
	g_mutex_unlock(playing_mutex);

	if (prev)
		put_song(prev);
}

static struct song *playlist_next(struct song *song, bool forward)
{
	g_mutex_lock(playlist_mutex);
	if (song->head.next) {
		struct list_head *pos;
		if (forward)
			pos = song->head.next;
		else
			pos = song->head.prev;
		if (pos != &playlist) {
			struct song *song = list_container(pos, struct song, head);
			get_song(song);
			g_mutex_unlock(playlist_mutex);
			return song;
		}
	}
	g_mutex_unlock(playlist_mutex);
	return NULL;
}

static gpointer playback_thread(gpointer ptr)
{
	UNUSED(ptr);

	static sample_t buffer[8192];

	ao_device *dev = NULL;
	struct input_plugin *plugin = NULL;
	struct input_plugin_ctx *ctx = NULL;
	ao_sample_format format = {.bits = 16, .byte_format = AO_FMT_NATIVE,};
	int power_cnt = 0, power = 0;

	while (true) {
		g_mutex_lock(playing_mutex);
		if (reset) {
			reset = false;
			g_mutex_unlock(playing_mutex);

			if (plugin) {
				plugin->close(ctx);
				g_free(ctx);
			}
			plugin = NULL;
		}
		g_mutex_unlock(playing_mutex);

		g_mutex_lock(play_mutex);
		if (!play) {
			g_cond_wait(play_cond, play_mutex);
			g_mutex_unlock(play_mutex);
			continue;
		}
		g_mutex_unlock(play_mutex);

		g_mutex_lock(playing_mutex);
		if (reset || !plugin) {
			struct song *song = playing;
			get_song(song);
			g_mutex_unlock(playing_mutex);

			if (plugin) {
				plugin->close(ctx);
				g_free(ctx);
			}

			plugin = detect_plugin(song->filename);
			if (!plugin) {
				put_song(song);
				play = false;
				continue;
			}

			ctx = g_malloc(plugin->ctx_size);
			if (!plugin->open(ctx, song->filename)) {
				put_song(song);
				g_free(ctx);
				plugin = NULL;
				play = false;
				continue;
			}
			put_song(song);
		}
		else
			g_mutex_unlock(playing_mutex);

		struct input_format iformat = {.rate = 0,};
		size_t len = plugin->fillbuf(ctx, buffer,
			sizeof(buffer) / sizeof(sample_t), &iformat);
		if (!len) {
			struct song *song = get_playing();
			struct song *next = playlist_next(song, true);
			put_song(song);
			if (next) {
				set_playing(next);
				put_song(next);
			} else
				play = false;
			continue;
		}

		if (iformat.rate != (unsigned int) format.rate ||
		    iformat.channels != (unsigned int) format.channels || !dev) {
			if (dev)
				ao_close(dev);
			printf("format change: %u Hz, %u channels\n",
				iformat.rate, iformat.channels);
			format.rate = iformat.rate;
			format.channels = iformat.channels;
			power_cnt = 0;
			power = 0;
			dev = ao_open_live(ao_default_driver_id(), &format, NULL);
			if (!dev) {
				printf("Unable to open audio device\n");
				play = false;
				continue;
			}
		}

		size_t i;
		for (i = power_cnt & 31; i < len; i += 32)
			power += abs(buffer[i]) / 256;

		power_cnt += len;
		if (power_cnt >= format.rate * format.channels / 16) {
			ui_set_power(power * 64 / power_cnt);
			power_cnt = 0;
			power = 0;
		}

		ao_play(dev, (char *)buffer, len * 2);
	}

	return NULL;
}

static const char *file_base(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/')
		--i;
	return &filename[i];
}

static char *build_filename(const char *orig, const char *filename)
{
	if (!filename[0])
		return NULL;
	if (!memcmp(filename, "http://", 7) || filename[0] == '/')
		return strdup(filename);

	size_t i = strlen(orig);
	while (i && orig[i - 1] != '/')
		--i;
	while (i && orig[i - 1] == '/')
		--i;

	char *buf = malloc(i + strlen(filename) + 2);
	if (!buf)
		return NULL;
	memcpy(buf, orig, i);
	buf[i] = '/';
	strcpy_q(&buf[i + 1], filename);
	return buf;
}

void add_playlist(struct song *song)
{
	get_song(song);

	g_mutex_lock(playlist_mutex);
	list_add_tail(&song->head, &playlist);
	ui_add_playlist(song);
	playlist_len++;
	g_mutex_unlock(playlist_mutex);
}

void remove_playlist(struct song *song)
{
	g_mutex_lock(playlist_mutex);
	list_del(&song->head);
	playlist_len--;
	memset(&song->head, 0, sizeof(song->head));
	ui_remove_playlist(song);
	g_mutex_unlock(playlist_mutex);

	put_song(song);
}

void clear_playlist()
{
	struct list_head *pos, *next;

	g_mutex_lock(playlist_mutex);
	list_for_each_safe(pos, next, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		memset(&song->head, 0, sizeof(song->head));
		ui_remove_playlist(song);
		put_song(song);
	}
	list_init(&playlist);
	playlist_len = 0;
	g_mutex_unlock(playlist_mutex);
}

void shuffle_playlist()
{
	struct list_head *pos;

	g_mutex_lock(playlist_mutex);
	struct song **table = g_new(struct song *, playlist_len);
	int len = 0;
	list_for_each(pos, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		int i = rand() % (len + 1);
		if (i != len)
			memmove(&table[i + 1], &table[i], (len - i) * sizeof(table[0]));
		table[i] = song;
		ui_remove_playlist(song);
		len++;
	}
	list_init(&playlist);
	int i;
	for (i = 0; i < playlist_len; ++i) {
		struct song *song = table[i];
		list_add_tail(&song->head, &playlist);
		ui_add_playlist(song);
	}
	g_free(table);
	g_mutex_unlock(playlist_mutex);
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

bool load_playlist_pls(const char *filename)
{
	char *playlist_name = absolute_path(filename);
	if (!playlist_name)
		return NULL;

	FILE *f = fopen(playlist_name, "r");
	if (!f) {
		free(playlist_name);
		return false;
	}

	char row[512];
	while (fgets(row, sizeof(row), f)) {
		size_t i;
		char *value = NULL;
		for (i = 0; row[i]; ++i) {
			if (row[i] == '=') {
				row[i] = 0;
				value = &row[i + 1];
				break;
			}
		}
		if (!memcmp(trim(row), "File", 4) && value) {
			char *fname = build_filename(playlist_name, trim(value));
			if (fname) {
				struct song *song = new_song(fname);
				if (song) {
					add_playlist(song);
					put_song(song);
				}
				free(fname);
			}
		}
	}
	free(playlist_name);
	fclose(f);
	return true;
}

bool load_playlist_m3u(const char *filename)
{
	char *playlist_name = absolute_path(filename);
	if (!playlist_name)
		return NULL;

	FILE *f = fopen(playlist_name, "r");
	if (!f) {
		free(playlist_name);
		return false;
	}

	char row[512];
	while (fgets(row, sizeof(row), f)) {
		if (row[0] != '#') {
			char *fname = build_filename(playlist_name, trim(row));
			if (fname) {
				struct song *song = new_song(fname);
				if (song) {
					add_playlist(song);
					put_song(song);
				}
				free(fname);
			}
		}
	}
	free(playlist_name);
	fclose(f);
	return true;
}

bool save_playlist_m3u(const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return false;

	struct list_head *pos;
	list_for_each(pos, &playlist) {
		struct song *song = list_container(pos, struct song, head);
		fputs(song->filename, f);
		fputc('\n', f);
	}
	fclose(f);
	return true;
}

static void kick_playback()
{
	g_mutex_lock(play_mutex);
	play = true;
	g_cond_signal(play_cond);
	g_mutex_unlock(play_mutex);

}

void play_playlist(struct song *song)
{
	set_playing(song);
	kick_playback();
}

void japlay_play()
{
	if (!playing) {
		g_mutex_lock(playlist_mutex);
		struct song *song = list_container(playlist.next,
					struct song, head);
		get_song(song);
		g_mutex_unlock(playlist_mutex);
		set_playing(song);
		put_song(song);
	}
	kick_playback();
}

void japlay_stop()
{
	reset = true;
	play = false;
}

void japlay_pause()
{
	play = false;
}

void japlay_skip()
{
	struct song *song = get_playing();
	struct song *next = playlist_next(song, true);
	put_song(song);
	if (next) {
		set_playing(next);
		put_song(next);
	}
}

static bool load_plugin(const char *filename)
{
	void *dl = dlopen(filename, RTLD_NOW);
	if (!dl)
		return false;

	struct input_plugin *(*get_info)() = dlsym(dl, "get_info");
	if (!get_info) {
		dlclose(dl);
		return false;
	}

	struct input_plugin *info = get_info();

	printf("found plugin: %s (%s)\n", file_base(filename), info->name);

	plugins = g_list_prepend(plugins, info);

	return true;
}

static void load_plugins()
{
	DIR *dir = opendir(PLUGIN_DIR);
	if (!dir)
		return;

	static const char plugin_dir[] = PLUGIN_DIR "/";

	struct dirent *de = readdir(dir);
	while (de) {
		if (de->d_name[0] != '.') {
			char *buf = malloc(strlen(plugin_dir) + strlen(de->d_name) + 1);
			if (buf) {
				strcpy_q(buf, plugin_dir);
				strcpy_q(&buf[strlen(plugin_dir)], de->d_name);
				if (!load_plugin(buf))
					printf("Unable to load plugin %s\n", de->d_name);
				free(buf);
			}
		}
		de = readdir(dir);
	}

	closedir(dir);
}

void japlay_init()
{
	ao_initialize();
	load_plugins();

	int i, count;
	ao_info **drivers = ao_driver_info_list(&count);
	for (i = 0; i < count; ++i) {
		if (drivers[i]->type == AO_TYPE_LIVE) {
			printf("ao driver: %s (%s)\n", drivers[i]->short_name,
				drivers[i]->name);
		}
	}

	list_init(&playlist);

	playlist_mutex = g_mutex_new();
	playing_mutex = g_mutex_new();

	play_mutex = g_mutex_new();
	play_cond = g_cond_new();
	g_thread_create(playback_thread, NULL, false, NULL);
}
