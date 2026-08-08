#include "fma/client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int set_timeout(int fd) {
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s SOCKET\n", argv[0]);
        return 2;
    }

    struct fma_client first;
    struct fma_client second;
    if (fma_client_connect(&first, argv[1]) < 0) {
        perror("connect first client");
        return 1;
    }
    if (set_timeout(first.socket_fd) < 0) {
        perror("set first client timeout");
        fma_client_close(&first);
        return 1;
    }
    struct fma_capabilities first_caps;
    if (fma_client_query_capabilities(&first, &first_caps) < 0) {
        perror("query first client");
        fma_client_close(&first);
        return 1;
    }

    alarm(2);
    if (fma_client_connect(&second, argv[1]) < 0) {
        alarm(0);
        perror("connect second client");
        fma_client_close(&first);
        return 1;
    }
    alarm(0);
    if (set_timeout(second.socket_fd) < 0) {
        perror("set second client timeout");
        fma_client_close(&second);
        fma_client_close(&first);
        return 1;
    }
    struct fma_capabilities second_caps;
    int result = fma_client_query_capabilities(&second, &second_caps);
    if (result < 0)
        fprintf(stderr, "second client was blocked: %s\n", strerror(errno));
    else if (second_caps.decoder_mask != first_caps.decoder_mask) {
        fprintf(stderr, "clients observed different decoder masks\n");
        result = -1;
    }

    fma_client_close(&second);
    fma_client_close(&first);
    if (result < 0)
        return 1;
    puts("two clients served concurrently");
    return 0;
}
