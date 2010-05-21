#include "iowatch.h"
#include "list.h"
#include <stdlib.h>
#include <sys/select.h>

struct iowatch {
	struct list_head head;
	int fd;
	io_watch_callback_t callback;
	void *ctx;
};

static struct list_head watches;

static fd_set watch_set;
static int max_fd;

struct iowatch *new_io_watch(int fd, io_watch_callback_t callback, void *ctx)
{
	struct iowatch *watch = calloc(1, sizeof(*watch));
	if (!watch)
		return NULL;
	watch->fd = fd;
	watch->callback = callback;
	watch->ctx = ctx;

	FD_SET(fd, &watch_set);
	if (fd > max_fd)
		max_fd = fd;

	list_add_tail(&watch->head, &watches);
	return watch;
}

void remove_io_watch(struct iowatch *watch)
{
	FD_CLR(watch->fd, &watch_set);

	list_del(&watch->head);
	free(watch);

	/* update maximum FD */
	if (watch->fd == max_fd) {
		max_fd = 0;
		struct list_head *pos;
		list_for_each(pos, &watches) {
			watch = list_container(pos, struct iowatch, head);
			if (watch->fd > max_fd)
				max_fd = watch->fd;
		}
	}
}

void *get_io_watch_context(struct iowatch *watch)
{
	return watch->ctx;
}

int get_io_watch_fd(struct iowatch *watch)
{
	return watch->fd;
}

void iowatch_select()
{
	if (!max_fd)
		return;

	if (select(max_fd + 1, &watch_set, NULL, NULL, NULL) < 0) {
		/* continue even if select fails */
		return;
	}

	struct list_head *pos, *next;
	list_for_each_safe(pos, next, &watches) {
		struct iowatch *watch = list_container(pos, struct iowatch, head);
		if (FD_ISSET(watch->fd, &watch_set))
			watch->callback(watch);
		else
			FD_SET(watch->fd, &watch_set);
	}
}

void init_iowatch()
{
	list_init(&watches);
	max_fd = 0;
	FD_ZERO(&watch_set);
}

void exit_iowatch()
{
	/* TO DO */
}
