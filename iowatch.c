#include "common.h"
#include "iowatch.h"
#include "list.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

#define MAX_PERIOD		3600

struct iowatch {
	struct list_head head;
	int fd;
	int flags;
	io_watch_callback_t callback;
	void *ctx;
};

struct timer {
	struct list_head head;
	time_t last_run, period;
	timer_callback_t callback;
	void *ctx;
};

static struct list_head watches, timers;
static fd_set in_set, out_set;
static int max_fd, min_period;

struct iowatch *new_io_watch(int fd, int flags, io_watch_callback_t callback,
			     void *ctx)
{
	struct iowatch *watch = NEW(struct iowatch);
	if (watch == NULL)
		return NULL;
	watch->fd = fd;
	watch->flags = flags;
	watch->callback = callback;
	watch->ctx = ctx;

	if (flags & IO_IN)
		FD_SET(fd, &in_set);
	if (flags & IO_OUT)
		FD_SET(fd, &out_set);
	if (fd > max_fd)
		max_fd = fd;

	list_add_tail(&watch->head, &watches);
	return watch;
}

void remove_io_watch(struct iowatch *watch)
{
	FD_CLR(watch->fd, &in_set);
	FD_CLR(watch->fd, &out_set);

	list_del(&watch->head);
	free(watch);

	/* update maximum fd */
	if (watch->fd == max_fd) {
		struct list_head *pos;

		max_fd = 0;
		list_for_each(pos, &watches) {
			watch = container_of(pos, struct iowatch, head);
			if (watch->fd > max_fd)
				max_fd = watch->fd;
		}
	}
}

void set_io_watch_flags(struct iowatch *watch, int flags)
{
	watch->flags = flags;
	if (flags & IO_IN)
		FD_SET(watch->fd, &in_set);
	else
		FD_CLR(watch->fd, &in_set);
	if (flags & IO_OUT)
		FD_SET(watch->fd, &out_set);
	else
		FD_CLR(watch->fd, &out_set);
}

struct timer *new_periodic_timer(int period, timer_callback_t callback,
				 void *ctx)
{
	struct timer *timer = NEW(struct timer);
	if (timer == NULL)
		return NULL;
	timer->last_run = time(NULL);
	timer->period = period;
	timer->callback = callback;
	timer->ctx = ctx;
	if (period < min_period)
		min_period = period;

	list_add_tail(&timer->head, &timers);
	return timer;
}

void remove_periodic_timer(struct timer *timer)
{
	list_del(&timer->head);
	free(timer);

	/* update minimum period */
	if (timer->period == min_period) {
		struct list_head *pos;

		min_period = MAX_PERIOD;
		list_for_each(pos, &timers) {
			timer = container_of(pos, struct timer, head);
			if (timer->period < min_period)
				min_period = timer->period;
		}
	}
}

void iowatch_poll(void)
{
	struct timeval tv = {.tv_sec = min_period / 2, .tv_usec = 0};
	struct list_head *pos, *next;
	time_t now;

	if (max_fd == 0) {
		warning("iowatch: No watches?\n");
		return;
	}

	if (select(max_fd + 1, &in_set, &out_set, NULL, &tv) < 0) {
		if (errno != EINTR)
			warning("select failed (%s)\n", strerror(errno));
		/* continue even if select fails */
		return;
	}

	list_for_each_safe(pos, next, &watches) {
		struct iowatch *watch = container_of(pos, struct iowatch, head);
		int flags = 0;

		if (FD_ISSET(watch->fd, &in_set))
			flags |= IO_IN;
		if (FD_ISSET(watch->fd, &out_set))
			flags |= IO_OUT;

		if (watch->flags & IO_IN)
			FD_SET(watch->fd, &in_set);
		if (watch->flags & IO_OUT)
			FD_SET(watch->fd, &out_set);

		if (flags) {
			if (watch->callback(watch->fd, flags, watch->ctx))
				remove_io_watch(watch);
		}
	}

	now = time(NULL);
	list_for_each_safe(pos, next, &timers) {
		struct timer *timer = container_of(pos, struct timer, head);
		if (now >= timer->last_run + timer->period) {
			timer->last_run = now;
			timer->callback(timer->ctx);
		}
	}
}

void iowatch_init(void)
{
	list_init(&watches);
	list_init(&timers);
	max_fd = 0;
	min_period = MAX_PERIOD;
	FD_ZERO(&in_set);
	FD_ZERO(&out_set);
}

void iowatch_exit(void)
{
	/* TODO */
}
