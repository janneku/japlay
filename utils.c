/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#define _GNU_SOURCE

#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>

char *concat_strings(const char *s, const char *t)
{
	char *buf;
	if (asprintf(&buf, "%s%s", s, t) < 0)
		return NULL;
	return buf;
}

char *get_config_dir(void)
{
	const char *home = getenv("HOME");
	if (home == NULL)
		return NULL;

	return concat_strings(home, "/.japlay");
}

/*
 * Return $HOME/.japlay/part, or NULL on error.
 */
char *get_config_name(const char *part)
{
	char *name;
	const char *home = getenv("HOME");
	if (home == NULL)
		return NULL;

	if (asprintf(&name, "%s/.japlay/%s", home, part) < 0)
		return NULL;

	return name;
}

char *absolute_path(const char *filename)
{
	if (!memcmp(filename, "http://", 7) || filename[0] == '/')
		return strdup(filename);

	char *cwd = getcwd(NULL, 0);
	if (cwd == NULL)
		return NULL;

	char *name;
	if (asprintf(&name, "%s/%s", cwd, filename) < 0)
		name = NULL;
	free(cwd);
	return name;
}

const char *file_base(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/')
		--i;
	return &filename[i];
}

char *file_dir(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/')
		--i;
	while (i && filename[i - 1] == '/')
		--i;
	return strndup(filename, i);
}

char *build_filename(const char *orig, const char *filename)
{
	if (!filename[0])
		return NULL;
	if (!memcmp(filename, "http://", 7) || filename[0] == '/')
		return strdup(filename);

	char *dir = file_dir(orig);
	if (dir == NULL)
		return NULL;

	char *name;
	if (asprintf(&name, "%s/%s", dir, filename) < 0)
		name = NULL;
	free(dir);
	return name;
}
