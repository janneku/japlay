#include "buffer.h"
#include "common.h"
#include <stdlib.h>

void init_buffer(struct audio_buffer *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->event = (size_t) -1;
	buf->wrap = (size_t) -1;
}

bool check_buffer_event(struct audio_buffer *buf)
{
	return (buf->tail == buf->event);
}

size_t buffer_read_avail(struct audio_buffer *buf)
{
	/* check for wrap around */
	if (buf->tail >= buf->wrap) {
		buf->tail = 0;
		buf->wrap = (size_t) -1;
	}

	size_t end = buf->wrap;
	if (buf->head < end && buf->head >= buf->tail)
		end = buf->head;
	if (buf->event < end && buf->event >= buf->tail)
		end = buf->event;

	return end - buf->tail;
}

int buffer_write_avail(struct audio_buffer *buf, size_t min_avail)
{
	if (buf->head < buf->tail)
		return buf->tail - buf->head - 1;

	size_t avail = BUFFER_LEN - buf->head;
	if (avail < min_avail && buf->tail != 0) {
		/* buffer wraps around */
		if (buf->head == buf->tail)
			avail = BUFFER_LEN;
		else
			avail = buf->tail - 1;
		buf->wrap = buf->head;
		buf->head = 0;
	}
	return avail;
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
	if (buf->tail == buf->event)
		buf->event = (size_t) -1;
	buf->tail += len;
}

void buffer_written(struct audio_buffer *buf, size_t len)
{
	buf->head += len;
}

void mark_buffer_event(struct audio_buffer *buf)
{
	buf->event = buf->head;
}
