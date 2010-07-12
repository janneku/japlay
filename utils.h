#ifndef _JAPLAY_UTILS_H_
#define _JAPLAY_UTILS_H_

char *concat_strings(const char *s, const char *t);
char *get_config_dir(void);
char *get_config_name(const char *part);
char *absolute_path(const char *filename);
const char *file_base(const char *filename);
char *file_dir(const char *filename);
char *build_filename(const char *orig, const char *filename);
char *trim(char *buf);

#endif
