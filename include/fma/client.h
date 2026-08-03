#ifndef FMA_CLIENT_H
#define FMA_CLIENT_H

#include "fma/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fma_client {
    int socket_fd;
    uint64_t next_request_id;
    uint64_t session_id;
};

int fma_client_connect(struct fma_client *client, const char *socket_path);
void fma_client_close(struct fma_client *client);
int fma_client_query_capabilities(struct fma_client *client,
                                  struct fma_capabilities *caps);
int fma_client_create_decoder(struct fma_client *client,
                              const struct fma_decoder_config *config,
                              struct fma_frame_pool *pool,
                              int *pool_fd);
int fma_client_queue_packet(struct fma_client *client, const void *data,
                            size_t size, int64_t pts_us, uint32_t flags);
int fma_client_queue_packet_to(struct fma_client *client, const void *data,
                               size_t size, int64_t pts_us, uint32_t flags,
                               int output_fd);
int fma_client_drain(struct fma_client *client);
int fma_client_flush(struct fma_client *client);
int fma_client_poll_output(struct fma_client *client, uint32_t timeout_ms);
int fma_client_release_frame(struct fma_client *client, uint32_t slot);
int fma_client_receive(struct fma_client *client, struct fma_message *message);

#ifdef __cplusplus
}
#endif

#endif
