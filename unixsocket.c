#include "common.h"
#include "unixsocket.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

static int unix_socket_initialize(struct sockaddr_un *addr, const char *path)
{
	int fd;
	int len;
	*addr = (struct sockaddr_un) {.sun_family = AF_UNIX};

	len = snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
	if (len >= (int)sizeof(addr->sun_path)) {
		warning("Too long a socket path: %s\n", path);
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		warning("Can not open socket\n");

	return fd;
}

int unix_socket_create(const char *path)
{
	struct sockaddr_un addr;
	int fd;
	int val = -1;

	fd = unix_socket_initialize(&addr, path);
	if (fd < 0)
		goto err;

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val) < 0) {
		warning("Can not re-use socket address (%s)\n",
			strerror(errno));
	}

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr))) {
		warning("Can not bind socket (%s)\n", strerror(errno));
		goto err;
	}

	if (listen(fd, 5)) {
		warning("Can not listen unix socket (%s)\n", strerror(errno));
		goto err;
	}

	return fd;

err:
	if (fd >= 0)
		close(fd);
	return -1;
}

int unix_socket_connect(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	fd = unix_socket_initialize(&addr, path);
	if (fd < 0)
		return -1;

	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr))) {
		warning("Can not connect (%s)\n", strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}
