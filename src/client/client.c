#include "fma/client.h"
#include "fma/transport.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int send_with_fd(struct fma_client *client, uint16_t type,
                        const void *payload, uint32_t payload_size,
                        int64_t pts_us, uint32_t flags, int passed_fd,
                        uint64_t *request_id) {
    struct fma_message message;
    fma_message_init(&message, type);
    message.flags = flags;
    message.payload = (uint8_t *)(uintptr_t)payload;
    message.payload_size = payload_size;
    message.request_id = client->next_request_id++;
    message.session_id = client->session_id;
    message.pts_us = pts_us;
    if (passed_fd >= 0) {
        message.fd_count = 1;
        message.fds[0] = passed_fd;
    }
    if (request_id)
        *request_id = message.request_id;
    return fma_send_message(client->socket_fd, &message);
}

static int send_simple(struct fma_client *client, uint16_t type,
                       const void *payload, uint32_t payload_size,
                       int64_t pts_us, uint32_t flags, uint64_t *request_id) {
    return send_with_fd(client, type, payload, payload_size, pts_us, flags, -1,
                        request_id);
}

static int expect(struct fma_client *client, uint16_t type, uint64_t request_id,
                  struct fma_message *reply) {
    for (;;) {
        if (fma_receive_message(client->socket_fd, reply) < 0)
            return -1;
        if (reply->type == FMA_MSG_ERROR) {
            fma_message_release(reply);
            errno = EPROTO;
            return -1;
        }
        if (reply->type == type && reply->request_id == request_id)
            return 0;
        fma_message_release(reply);
    }
}

int fma_client_connect(struct fma_client *client, const char *socket_path) {
    memset(client, 0, sizeof(*client));
    client->socket_fd = fma_connect_unix(socket_path);
    client->next_request_id = 1;
    if (client->socket_fd < 0)
        return -1;

    uint8_t hello[4];
    fma_put_u16(hello + 0, FMA_PROTOCOL_MAJOR);
    fma_put_u16(hello + 2, FMA_PROTOCOL_MINOR);
    uint64_t request_id;
    if (send_simple(client, FMA_MSG_HELLO, hello, sizeof(hello), 0, 0,
                    &request_id) < 0) {
        fma_client_close(client);
        return -1;
    }
    struct fma_message reply;
    if (expect(client, FMA_MSG_HELLO_REPLY, request_id, &reply) < 0) {
        fma_client_close(client);
        return -1;
    }
    client->session_id = reply.session_id;
    fma_message_release(&reply);
    return 0;
}

void fma_client_close(struct fma_client *client) {
    if (client->socket_fd >= 0) {
        (void)send_simple(client, FMA_MSG_CLOSE, NULL, 0, 0, 0, NULL);
        close(client->socket_fd);
    }
    client->socket_fd = -1;
}

int fma_client_query_capabilities(struct fma_client *client,
                                  struct fma_capabilities *caps) {
    uint64_t request_id;
    if (send_simple(client, FMA_MSG_QUERY_CAPABILITIES, NULL, 0, 0, 0,
                    &request_id) < 0)
        return -1;
    struct fma_message reply;
    if (expect(client, FMA_MSG_CAPABILITIES, request_id, &reply) < 0)
        return -1;
    int result = fma_decode_capabilities(reply.payload, reply.payload_size, caps);
    fma_message_release(&reply);
    return result;
}

int fma_client_create_decoder(struct fma_client *client,
                              const struct fma_decoder_config *config,
                              struct fma_frame_pool *pool, int *pool_fd) {
    uint8_t payload[16];
    fma_encode_decoder_config(config, payload);
    uint64_t request_id;
    if (send_simple(client, FMA_MSG_CREATE_DECODER, payload, sizeof(payload),
                    0, 0, &request_id) < 0)
        return -1;

    struct fma_message reply;
    if (expect(client, FMA_MSG_DECODER_READY, request_id, &reply) < 0)
        return -1;
    client->session_id = reply.session_id;
    fma_message_release(&reply);

    if (fma_receive_message(client->socket_fd, &reply) < 0)
        return -1;
    if (reply.type != FMA_MSG_FRAME_POOL || reply.fd_count != 1 ||
        fma_decode_frame_pool(reply.payload, reply.payload_size, pool) < 0) {
        fma_message_release(&reply);
        errno = EPROTO;
        return -1;
    }
    *pool_fd = reply.fds[0];
    reply.fds[0] = -1;
    reply.fd_count = 0;
    fma_message_release(&reply);
    return 0;
}

int fma_client_queue_packet(struct fma_client *client, const void *data,
                            size_t size, int64_t pts_us, uint32_t flags) {
    return fma_client_queue_packet_to(client, data, size, pts_us, flags, -1);
}

int fma_client_queue_packet_to(struct fma_client *client, const void *data,
                               size_t size, int64_t pts_us, uint32_t flags,
                               int output_fd) {
    if (!data || size == 0 || size > FMA_MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }
    return send_with_fd(client, FMA_MSG_QUEUE_PACKET, data, (uint32_t)size,
                        pts_us, flags, output_fd, NULL);
}

int fma_client_drain(struct fma_client *client) {
    return send_simple(client, FMA_MSG_DRAIN, NULL, 0, 0, 0, NULL);
}

int fma_client_flush(struct fma_client *client) {
    uint64_t request_id;
    if (send_simple(client, FMA_MSG_FLUSH, NULL, 0, 0, 0, &request_id) < 0)
        return -1;
    struct fma_message reply;
    if (expect(client, FMA_MSG_FLUSHED, request_id, &reply) < 0)
        return -1;
    fma_message_release(&reply);
    return 0;
}

int fma_client_poll_output(struct fma_client *client, uint32_t timeout_ms) {
    uint8_t payload[4];
    fma_put_u32(payload, timeout_ms);
    return send_simple(client, FMA_MSG_POLL_OUTPUT, payload, sizeof(payload),
                       0, 0, NULL);
}

int fma_client_release_frame(struct fma_client *client, uint32_t slot) {
    uint8_t payload[4];
    fma_put_u32(payload, slot);
    return send_simple(client, FMA_MSG_RELEASE_FRAME, payload, sizeof(payload),
                       0, 0, NULL);
}

int fma_client_receive(struct fma_client *client, struct fma_message *message) {
    return fma_receive_message(client->socket_fd, message);
}
