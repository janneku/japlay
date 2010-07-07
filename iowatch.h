#ifndef _LCN_IOWATCH_H_
#define _LCN_IOWATCH_H_

struct iowatch;
struct timer;

#define IO_IN		1
#define IO_OUT		2

typedef void (*timer_callback_t)(void *ctx);
typedef int (*io_watch_callback_t)(int fd, int flags, void *ctx);

struct iowatch *new_io_watch(int fd, int flags, io_watch_callback_t callback,
			     void *ctx);
void remove_io_watch(struct iowatch *watch);
void set_io_watch_flags(struct iowatch *watch, int flags);

struct timer *new_periodic_timer(int period, timer_callback_t callback,
				 void *ctx);
void remove_periodic_timer(struct timer *timer);

void iowatch_poll(void);
void iowatch_init(void);
void iowatch_exit(void);

#endif
