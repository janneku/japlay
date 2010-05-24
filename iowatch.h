struct iowatch;

typedef void (*io_watch_callback_t)(struct iowatch *watch);

struct iowatch *new_io_watch(int fd, io_watch_callback_t callback, void *ctx);
void remove_io_watch(struct iowatch *watch);

/* Getters: */
void *get_io_watch_context(struct iowatch *watch);
int get_io_watch_fd(struct iowatch *watch);

void iowatch_select();
void init_iowatch();
void exit_iowatch();
