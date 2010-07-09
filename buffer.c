#include "buffer.h"
#include "common.h"
#include <stdlib.h>

void init_buffer(struct audio_buffer *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->formatchg = (size_t) -1;
}

bool check_buffer_formatchg(struct audio_buffer *buf)
{
	if (buf->tail == buf->formatchg) {
		buf->formatchg = (size_t) -1;
		return true;
	}
	return false;
}

size_t buffer_read_avail(struct audio_buffer *buf)
{
	if (buf->tail <= buf->head)
		return buf->head - buf->tail;
	return buf->wrap - buf->tail;
}

int buffer_write_avail(struct audio_buffer *buf, size_t min_avail)
{
	if (buf->head < buf->tail)
		return buf->tail - buf->head - 1;

	size_t max = BUFFER_LEN - buf->head;
	if (max < min_avail && buf->tail != 0) {
		/* buffer wraps around */
		if (buf->head == buf->tail) {
			max = BUFFER_LEN;
			buf->tail = 0;
		} else
			max = buf->tail - 1;
		buf->wrap = buf->head;
		buf->head = 0;
	}
	return max;
}

sample_t *read_buffer(struct audio_buffer *buf)
{
	return &buf->data[buf->tail];
}

sample_t *write_buffer(struct audio_buffer *buf)
{
	return &buf->data[buf->head];
}

void buffer_processed(struct audio_buffer *buf, size_t len)
{
	buf->tail += len;
	/* check for wrap around */
	if (buf->head < buf->tail && buf->tail >= buf->wrap)
		buf->tail = 0;
}

void buffer_written(struct audio_buffer *buf, size_t len)
{
	buf->head += len;
}

void mark_buffer_formatchg(struct audio_buffer *buf)
{
	buf->formatchg = buf->head;
}
