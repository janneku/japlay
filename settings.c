/*
 * japlay - Just Another Player
 * Copyright Janne Kulmala 2010
 */
#include "settings.h"
#include "common.h"
#include "list.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>

/* Generic settings parser and serializer */
/* NOTE: this is not thread safe! */

static struct list_head settings;

struct settings_entry {
	struct list_head head;
	char *name;
	char *val;
};

static struct settings_entry *find_setting(const char *name)
{
	struct list_head *pos;
	list_for_each(pos, &settings) {
		struct settings_entry *entry =
			container_of(pos, struct settings_entry, head);
		if (strcmp(entry->name, name) == 0)
			return entry;
	}
	return NULL;
}

const char *get_setting(const char *name)
{
	struct settings_entry *entry = find_setting(name);
	if (entry == NULL)
		return NULL;
	return entry->val;
}

int set_setting(const char *name, const char *val)
{
	struct settings_entry *entry = find_setting(name);
	if (entry == NULL) {
		entry = NEW(struct settings_entry);
		if (entry == NULL)
			return -1;
		entry->name = strdup(name);
		list_add_tail(&entry->head, &settings);
	}
	free(entry->val);
	entry->val = strdup(val);
	return 0;
}

int load_settings(const char *filename)
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
		trim(row);
		trim(value);
		set_setting(row, value);
	}
	fclose(f);
	return 0;
}

int save_settings(const char *filename)
{
	FILE *f = fopen(filename, "w");
	if (!f)
		return -1;

	struct list_head *pos;
	list_for_each(pos, &settings) {
		struct settings_entry *entry =
			container_of(pos, struct settings_entry, head);
		fprintf(f, "%s = %s\n", entry->name, entry->val);
	}
	fclose(f);
	return 0;
}

void init_settings(void)
{
	list_init(&settings);
}
