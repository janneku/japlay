/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define strcpy_q(d, s)		\
	memcpy(d, s, strlen(s) + 1)

char *get_cwd()
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

char *absolute_path(const char *filename)
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
	strcpy_q(&buf[i + 1], filename);
	return buf;
}
