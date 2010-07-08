#ifndef _JAPLAY_BUFFER_H_
#define _JAPLAY_BUFFER_H_

#include <string.h> /* size_t */
#include "plugin.h"

#define BUFFER_LEN		0x8000

struct audio_buffer {
	size_t head, tail, wrap, formatchg;
	sample_t data[BUFFER_LEN];
};

void init_buffer(struct audio_buffer *buf);
bool check_buffer_formatchg(struct audio_buffer *buf);
size_t buffer_read_avail(struct audio_buffer *buf);
int buffer_write_avail(struct audio_buffer *buf, size_t min_avail);
const sample_t *read_buffer(struct audio_buffer *buf);
sample_t *write_buffer(struct audio_buffer *buf);
void buffer_processed(struct audio_buffer *buf, size_t len);
void buffer_written(struct audio_buffer *buf, size_t len);
void mark_buffer_formatchg(struct audio_buffer *buf);


#endif
