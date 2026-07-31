#ifndef FMA_TRANSPORT_H
#define FMA_TRANSPORT_H

#include "fma/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int fma_connect_unix(const char *path);
int fma_listen_unix(const char *path, int backlog);
int fma_send_message(int socket_fd, const struct fma_message *message);
int fma_receive_message(int socket_fd, struct fma_message *message);

#ifdef __cplusplus
}
#endif

#endif
