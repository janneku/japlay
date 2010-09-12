#ifndef _SETTINGS_H_
#define _SETTINGS_H_

const char *get_setting(const char *name);
int set_setting(const char *name, const char *val);
int load_settings(const char *filename);
int save_settings(const char *filename);
void init_settings(void);

#endif
