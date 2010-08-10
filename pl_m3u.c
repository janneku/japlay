/*
 * japlay m3u playlist loader plugin
 * Copyright Janne Kulmala 2010
 */
#include "common.h"
#include "japlay.h"
#include "playlist.h"
#include "utils.h"
#include "plugin.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static bool m3u_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	return ext && !strcasecmp(ext, "m3u");
}

static int m3u_load(struct playlist *playlist, const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return -1;

	/* info extension */
	char *title = NULL;
	unsigned int length = -1;

	char row[512];
	while (fgets(row, sizeof(row), f)) {
		const char extinf[] = "#EXTINF:";

		if (row[0] != '#') {
			char *fname = build_filename(filename, trim(row));
			if (fname == NULL)
				continue;
			struct playlist_entry *entry =
				add_file_playlist(playlist, fname);
			if (entry) {
				struct song *song = get_entry_song(entry);
				if (title)
					set_song_title(song, title);
				if (length != (unsigned int) -1)
					set_song_length(song, length * 1000, 20);
				put_entry(entry);
			}
			free(title);
			free(fname);
			title = NULL;
			length = -1;

		} else if(!strncmp(row, extinf, strlen(extinf))) {
			char *comma = strchr(row, ',');
			if (comma) {
				*comma = 0;
				comma++;
			}
			char *len = &row[strlen(extinf)];
			if (isdigit(*len))
				length = atoi(len);
			if (comma) {
				free(title);
				title = strdup(trim(comma));
			}
		}
	}
	free(title);
	fclose(f);
	return 0;
}

static struct playlist_plugin plugin_info = {
	.size = sizeof(struct playlist_plugin),
	.name = "m3u playlist loader",
	.detect = m3u_detect,
	.load = m3u_load,
};

struct playlist_plugin *get_playlist_plugin()
{
	return &plugin_info;
}
