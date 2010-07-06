#ifndef _LCN_UNIXSOCKET_H_
#define _LCN_UNIXSOCKET_H_

int unix_socket_create(const char *path);
int unix_socket_connect(const char *path);

#endif
