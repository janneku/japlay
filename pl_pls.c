/*
 * japlay pls playlist loader plugin
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

static bool pls_detect(const char *filename)
{
	const char *ext = file_ext(filename);
	return ext && !strcasecmp(ext, "pls");
}

static int pls_load(struct playlist *playlist, const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (!f)
		return -1;

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
				struct playlist_entry *entry
					= add_file_playlist(playlist, fname);
				if (entry)
					put_entry(entry);
				free(fname);
			}
		}
	}
	fclose(f);
	return 0;
}

static struct playlist_plugin plugin_info = {
	.size = sizeof(struct playlist_plugin),
	.name = "pls playlist loader",
	.detect = pls_detect,
	.load = pls_load,
};

struct playlist_plugin *get_playlist_plugin()
{
	return &plugin_info;
}
