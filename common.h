#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)(x)

#define error(fmt...)		fprintf(stderr, "ERROR: " fmt)
#define warning(fmt...)		fprintf(stderr, "WARNING: " fmt)
#define info(fmt...)		do { \
					if (japlay_debug) \
						fprintf(stderr, "INFO: " fmt); \
				} while (0)

#define NEW(type)	(type *)calloc(1, sizeof(type))

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member) \
	((type *) ((char *) (ptr) - offsetof(type, member)))

extern int japlay_debug;

#endif
