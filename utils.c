/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#define _GNU_SOURCE /* asprintf */

#include "utils.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>

char *concat_strings(const char *s, const char *t)
{
	char *buf;
	if (asprintf(&buf, "%s%s", s, t) < 0)
		return NULL;
	return buf;
}

char *concat_path(const char *s, const char *t)
{
	char *buf;
	if (asprintf(&buf, "%s/%s", s, t) < 0)
		return NULL;
	return buf;
}

size_t str_hash(const char *str)
{
	size_t hash = 5381;
	while (*str)
		hash = ((hash << 5) + hash) + *str++;
	return hash;
}

char *get_config_dir(void)
{
	const char *home = getenv("HOME");
	if (home == NULL)
		return NULL;

	return concat_path(home, ".japlay");
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

	char *name = concat_path(cwd, filename);
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

const char *file_ext(const char *filename)
{
	size_t i = strlen(filename);
	while (i && filename[i - 1] != '/') {
		if (filename[i - 1] == '.')
			return &filename[i];
		--i;
	}
	return NULL;
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

	char *name = concat_path(dir, filename);
	free(dir);
	return name;
}

ssize_t read_in_full(int fd, void *buf, size_t count)
{
	char *p = buf;
	ssize_t ret;
	ssize_t bytes = 0;

	while (count > 0) {
		ret = read(fd, p + bytes, count);
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return bytes ? bytes : -1;
		} else if (ret == 0) {
			return bytes;
		}
		count -= ret;
		bytes += ret;
	}

	return bytes;
}

ssize_t xread(int fd, void *buf, size_t maxlen)
{
	ssize_t ret;
	while (true) {
		ret = read(fd, buf, maxlen);
		if (ret < 0 && errno == EINTR)
			continue;
		return ret;
	}
}

int setblocking(int fd, bool blocking)
{
	int flags = fcntl(fd, F_GETFL);
	flags &= ~O_NONBLOCK;
	if (!blocking)
		flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

int wait_on_socket(int fd, bool for_recv, int timeout_ms)
{
	struct timeval tv = {.tv_sec = timeout_ms / 1000,
			     .tv_usec = (timeout_ms % 1000) * 1000};

	fd_set infd, outfd;
	FD_ZERO(&infd);
	FD_ZERO(&outfd);
	if (for_recv)
		FD_SET(fd, &infd);
	else
		FD_SET(fd, &outfd);

	while (true) {
		int ret = select(fd + 1, &infd, &outfd, NULL, &tv);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret == 0) {
			/* timeout */
			errno = EAGAIN;
			return -1;
		} else if (ret > 0)
			return 0;
		return ret;
	}
}

/*
 * Strip leading and trailing spaces from the string. The operation is done
 * in place.
 */
void trim(char *buf)
{
	char *p = buf;
	while (isspace(*p))
		++p;
	char *end = &p[strlen(p)];
	while (end > p && isspace(end[-1]))
		--end;
	*end = 0;
	memmove(buf, p, end + 1 - p);
}

char *strdup(const char *str)
{
	size_t n = strlen(str) + 1;
	char *dup = malloc(n);
	if(dup)
		memcpy(dup, str, n);
	return dup;
}

int strcasecmp(const char *a, const char *b)
{
	while (*a || *b) {
		if (tolower(*a) != tolower(*b))
			return *b - *a;
		a++;
		b++;
	}
	return 0;
}
