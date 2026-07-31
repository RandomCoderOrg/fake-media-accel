#include "fma/protocol.h"
#include "fma/transport.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct fake_session {
    uint64_t id;
    struct fma_frame_pool pool;
    int pool_fd;
    uint8_t *pool_map;
    size_t pool_bytes;
    uint32_t next_slot;
    bool slots[FMA_MAX_SLOTS];
};

static volatile sig_atomic_t running = 1;

static void on_signal(int signal_number) {
    (void)signal_number;
    running = 0;
}

static int create_shared_file(size_t size) {
    char path[] = "/tmp/fma-pool-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int send_reply(int fd, const struct fma_message *request, uint16_t type,
                      const void *payload, uint32_t payload_size,
                      uint64_t session_id, int passed_fd) {
    struct fma_message reply;
    fma_message_init(&reply, type);
    reply.request_id = request ? request->request_id : 0;
    reply.session_id = session_id;
    reply.pts_us = request ? request->pts_us : 0;
    reply.payload = (uint8_t *)(uintptr_t)payload;
    reply.payload_size = payload_size;
    if (passed_fd >= 0) {
        reply.fd_count = 1;
        reply.fds[0] = passed_fd;
    }
    return fma_send_message(fd, &reply);
}

static int send_error(int fd, const struct fma_message *request, const char *text) {
    (void)send_reply(fd, request, FMA_MSG_ERROR, text, (uint32_t)strlen(text),
                     request ? request->session_id : 0, -1);
    errno = EPROTO;
    return -1;
}

static void destroy_pool(struct fake_session *session) {
    if (session->pool_map && session->pool_map != MAP_FAILED)
        munmap(session->pool_map, session->pool_bytes);
    if (session->pool_fd >= 0)
        close(session->pool_fd);
    session->pool_map = NULL;
    session->pool_fd = -1;
    session->pool_bytes = 0;
}

static int create_pool(struct fake_session *session,
                       const struct fma_decoder_config *config) {
    destroy_pool(session);
    uint32_t stride = (config->width + 63u) & ~63u;
    session->pool = (struct fma_frame_pool) {
        .pixel_format = FMA_PIXFMT_NV12,
        .width = config->width,
        .height = config->height,
        .stride = stride,
        .slot_count = config->slot_count,
        .slot_size = stride * config->height * 3u / 2u,
    };
    session->pool_bytes = (size_t)session->pool.slot_size * session->pool.slot_count;
    if (session->pool_bytes > FMA_MAX_POOL_BYTES) {
        errno = E2BIG;
        return -1;
    }
    session->pool_fd = create_shared_file(session->pool_bytes);
    if (session->pool_fd < 0)
        return -1;
    session->pool_map = mmap(NULL, session->pool_bytes, PROT_READ | PROT_WRITE,
                             MAP_SHARED, session->pool_fd, 0);
    if (session->pool_map == MAP_FAILED) {
        session->pool_map = NULL;
        destroy_pool(session);
        return -1;
    }
    memset(session->slots, 0, sizeof(session->slots));
    session->next_slot = 0;
    return 0;
}

static int emit_frame(int fd, const struct fma_message *request,
                      struct fake_session *session) {
    uint32_t slot = session->next_slot;
    for (uint32_t i = 0; i < session->pool.slot_count; ++i) {
        slot = (session->next_slot + i) % session->pool.slot_count;
        if (!session->slots[slot])
            goto found;
    }
    return send_error(fd, request, "frame pool exhausted");

found:
    session->slots[slot] = true;
    session->next_slot = (slot + 1) % session->pool.slot_count;
    uint8_t *pixels = session->pool_map + (size_t)slot * session->pool.slot_size;
    for (uint32_t y = 0; y < session->pool.height; ++y)
        memset(pixels + (size_t)y * session->pool.stride,
               (uint8_t)(16 + request->request_id % 220), session->pool.width);
    uint8_t *uv = pixels + (size_t)session->pool.stride * session->pool.height;
    memset(uv, 128, (size_t)session->pool.stride * session->pool.height / 2);

    struct fma_frame frame = {
        .slot = slot,
        .bytes_used = session->pool.slot_size,
        .width = session->pool.width,
        .height = session->pool.height,
        .stride = session->pool.stride,
        .pixel_format = session->pool.pixel_format,
    };
    uint8_t payload[24];
    fma_encode_frame(&frame, payload);
    return send_reply(fd, request, FMA_MSG_FRAME_READY, payload, sizeof(payload),
                      session->id, -1);
}

static int handle_client(int fd) {
    struct fake_session session = {.id = 1, .pool_fd = -1};
    int result = 0;
    while (running) {
        struct fma_message request;
        if (fma_receive_message(fd, &request) < 0)
            break;

        if (request.type != FMA_MSG_HELLO && request.session_id != session.id) {
            result = send_error(fd, &request, "invalid session");
            fma_message_release(&request);
            break;
        }

        switch (request.type) {
        case FMA_MSG_HELLO:
            if (request.payload_size != 4 ||
                fma_get_u16(request.payload) != FMA_PROTOCOL_MAJOR)
                result = send_error(fd, &request, "incompatible protocol");
            else
                result = send_reply(fd, &request, FMA_MSG_HELLO_REPLY, NULL, 0,
                                    session.id, -1);
            break;
        case FMA_MSG_QUERY_CAPABILITIES: {
            struct fma_capabilities caps = {
                .decoder_mask = FMA_CODEC_BIT(FMA_CODEC_H264) |
                                FMA_CODEC_BIT(FMA_CODEC_HEVC) |
                                FMA_CODEC_BIT(FMA_CODEC_VP8) |
                                FMA_CODEC_BIT(FMA_CODEC_VP9) |
                                FMA_CODEC_BIT(FMA_CODEC_AV1),
                .pixel_format_mask = 1u << (FMA_PIXFMT_NV12 - 1u),
                .flags = FMA_CAP_SHARED_FRAME_POOL | FMA_CAP_CAN_FLUSH |
                         FMA_CAP_CAN_POLL,
                .max_width = 8192,
                .max_height = 8192,
            };
            uint8_t payload[20];
            fma_encode_capabilities(&caps, payload);
            result = send_reply(fd, &request, FMA_MSG_CAPABILITIES, payload,
                                sizeof(payload), session.id, -1);
            break;
        }
        case FMA_MSG_CREATE_DECODER: {
            struct fma_decoder_config config;
            if (fma_decode_decoder_config(request.payload, request.payload_size,
                                          &config) < 0 ||
                create_pool(&session, &config) < 0) {
                result = send_error(fd, &request, "invalid decoder configuration");
                break;
            }
            result = send_reply(fd, &request, FMA_MSG_DECODER_READY, NULL, 0,
                                session.id, -1);
            if (result == 0) {
                uint8_t payload[24];
                fma_encode_frame_pool(&session.pool, payload);
                result = send_reply(fd, NULL, FMA_MSG_FRAME_POOL, payload,
                                    sizeof(payload), session.id, session.pool_fd);
            }
            break;
        }
        case FMA_MSG_QUEUE_PACKET:
            result = emit_frame(fd, &request, &session);
            if (result == 0)
                result = send_reply(fd, &request, FMA_MSG_PACKET_ACK, NULL, 0,
                                    session.id, -1);
            break;
        case FMA_MSG_RELEASE_FRAME: {
            uint32_t slot = request.payload_size == 4 ? fma_get_u32(request.payload) : 16;
            if (slot < session.pool.slot_count)
                session.slots[slot] = false;
            else
                result = send_error(fd, &request, "invalid frame slot");
            break;
        }
        case FMA_MSG_FLUSH:
            memset(session.slots, 0, sizeof(session.slots));
            result = send_reply(fd, &request, FMA_MSG_FLUSHED, NULL, 0,
                                session.id, -1);
            break;
        case FMA_MSG_DRAIN:
            result = send_reply(fd, &request, FMA_MSG_OUTPUT_EOS, NULL, 0,
                                session.id, -1);
            break;
        case FMA_MSG_POLL_OUTPUT:
            if (request.payload_size != 4)
                result = send_error(fd, &request, "invalid poll request");
            else
                result = send_reply(fd, &request, FMA_MSG_POLL_DONE, NULL, 0,
                                    session.id, -1);
            break;
        case FMA_MSG_CLOSE:
            fma_message_release(&request);
            destroy_pool(&session);
            return 0;
        default:
            result = send_error(fd, &request, "unsupported message");
            break;
        }
        fma_message_release(&request);
        if (result < 0)
            break;
    }
    destroy_pool(&session);
    return result;
}

int main(int argc, char **argv) {
    const char *socket_path = argc >= 2 ? argv[1] : "/tmp/fake-media-accel.sock";
    bool once = argc >= 3 && strcmp(argv[2], "--once") == 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int server = fma_listen_unix(socket_path, 4);
    if (server < 0) {
        perror("listen");
        return 1;
    }
    printf("fake-media-acceld listening on %s\n", socket_path);
    fflush(stdout);
    while (running) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        (void)handle_client(client);
        close(client);
        if (once)
            break;
    }
    close(server);
    unlink(socket_path);
    return 0;
}
