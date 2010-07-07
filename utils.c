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

char *get_cwd(void)
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

	char *cwd = get_cwd();
	if (!cwd)
		return NULL;

	char *name = concat_strings(cwd, filename);
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

char *build_filename(const char *orig, const char *filename)
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
	memcpy(&buf[i + 1], filename, strlen(filename) + 1);
	return buf;
}
