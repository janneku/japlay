#ifndef _JAPLAY_UTILS_H_
#define _JAPLAY_UTILS_H_

#include <string.h> /* size_t */
#include <unistd.h> /* ssize_t */
#include <stdbool.h>

#undef strdup /* glibc braindamage */

char *concat_strings(const char *s, const char *t);
char *concat_path(const char *s, const char *t);
size_t str_hash(const char *str);
char *get_config_dir(void);
char *get_config_name(const char *part);
char *absolute_path(const char *filename);
const char *file_base(const char *filename);
const char *file_ext(const char *filename);
char *file_dir(const char *filename);
char *build_filename(const char *orig, const char *filename);
ssize_t read_in_full(int fd, void *buf, size_t count);
ssize_t xread(int fd, void *buf, size_t maxlen);
int setblocking(int fd, bool blocking);
int wait_on_socket(int fd, bool for_recv, int timeout_ms);
char *trim(char *buf);
char *strdup(const char *str);
int strcasecmp(const char *a, const char *b);

#endif
