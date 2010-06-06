#ifndef _IOWATCH_H_
#define _IOWATCH_H_

struct iowatch;

typedef int (*io_watch_callback_t)(int fd, void *ctx);

struct iowatch *new_io_watch(int fd, io_watch_callback_t callback, void *ctx);
void remove_io_watch(struct iowatch *watch);

void iowatch_select(void);
void init_iowatch(void);
void exit_iowatch(void);

#endif
