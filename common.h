#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>

#define APP_NAME		"japlay"

#define UNUSED(x)		(void)(x)

#define warning(fmt...)		fprintf(stderr, "WARNING: " fmt)
#define info(fmt...)		if(debug) fprintf(stderr, "INFO: " fmt)

#define NEW(type)	(type *)calloc(1, sizeof(type))

extern int debug;

#endif
