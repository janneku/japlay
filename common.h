#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)(x)

#define error(...)		fprintf(stderr, "ERROR: " __VA_ARGS__)
#define linewarning(fmt, ...)	fprintf(stderr, "WARNING: %s:%d (%s): " fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define warning(...)		fprintf(stderr, "WARNING: " __VA_ARGS__)
#define info(...)		do { \
					if (japlay_debug) \
						fprintf(stderr, "INFO: " __VA_ARGS__); \
				} while (0)

#define NEW(type)	(type *)calloc(1, sizeof(type))

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member) \
	((type *) ((char *) (ptr) - offsetof(type, member)))

extern int japlay_debug;

#endif
