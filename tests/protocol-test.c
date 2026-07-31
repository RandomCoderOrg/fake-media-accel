#include "fma/protocol.h"
#include "fma/transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void test_header_roundtrip(void) {
    struct fma_message source;
    fma_message_init(&source, FMA_MSG_FRAME_READY);
    source.flags = 7;
    source.payload_size = 24;
    source.fd_count = 1;
    source.request_id = UINT64_C(0x1020304050607080);
    source.session_id = UINT64_C(0x8877665544332211);
    source.pts_us = -42;
    uint8_t wire[FMA_WIRE_HEADER_SIZE];
    CHECK(fma_encode_header(&source, wire) == 0);
    struct fma_message decoded;
    CHECK(fma_decode_header(wire, &decoded) == 0);
    CHECK(decoded.type == source.type);
    CHECK(decoded.flags == source.flags);
    CHECK(decoded.payload_size == source.payload_size);
    CHECK(decoded.fd_count == source.fd_count);
    CHECK(decoded.request_id == source.request_id);
    CHECK(decoded.session_id == source.session_id);
    CHECK(decoded.pts_us == source.pts_us);
}

static void test_rejects_bad_wire(void) {
    uint8_t wire[FMA_WIRE_HEADER_SIZE] = {0};
    struct fma_message message;
    CHECK(fma_decode_header(wire, &message) == -1);
    CHECK(errno == EPROTO);
}

static void test_rejects_unbounded_decoder(void) {
    struct fma_decoder_config config = {
        .codec = FMA_CODEC_H264,
        .width = FMA_MAX_DIMENSION + 1,
        .height = 1080,
        .slot_count = FMA_DEFAULT_SLOTS,
    };
    uint8_t wire[16];
    fma_encode_decoder_config(&config, wire);
    struct fma_decoder_config decoded;
    CHECK(fma_decode_decoder_config(wire, sizeof(wire), &decoded) == -1);
    CHECK(errno == EINVAL);
}

static void test_payload_and_fd_roundtrip(void) {
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int null_fd = open("/dev/null", O_RDONLY);
    CHECK(null_fd >= 0);
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    struct fma_message source;
    fma_message_init(&source, FMA_MSG_FRAME_POOL);
    source.payload = (uint8_t *)(uintptr_t)payload;
    source.payload_size = sizeof(payload);
    source.fd_count = 1;
    source.fds[0] = null_fd;
    CHECK(fma_send_message(sockets[0], &source) == 0);
    struct fma_message received;
    CHECK(fma_receive_message(sockets[1], &received) == 0);
    CHECK(received.type == FMA_MSG_FRAME_POOL);
    CHECK(received.payload_size == sizeof(payload));
    CHECK(memcmp(received.payload, payload, sizeof(payload)) == 0);
    CHECK(received.fd_count == 1 && received.fds[0] >= 0);
    fma_message_release(&received);
    close(null_fd);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void) {
    test_header_roundtrip();
    test_rejects_bad_wire();
    test_rejects_unbounded_decoder();
    test_payload_and_fd_roundtrip();
    puts("protocol tests passed");
    return 0;
}
