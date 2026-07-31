#include "fma/transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#define FMA_SET_RECEIVED_CLOEXEC 1
#endif

static int make_address(const char *path, struct sockaddr_un *address,
                        socklen_t *address_length) {
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(address->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path[0] == '@') {
#if defined(__linux__)
        memcpy(address->sun_path + 1, path + 1, length - 1);
        *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length);
#else
        errno = EAFNOSUPPORT;
        return -1;
#endif
    } else {
        memcpy(address->sun_path, path, length + 1);
        *address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    }
    return 0;
}

int fma_connect_unix(const char *path) {
    struct sockaddr_un address;
    socklen_t address_length;
    if (make_address(path, &address, &address_length) < 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&address, address_length) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int fma_listen_unix(const char *path, int backlog) {
    struct sockaddr_un address;
    socklen_t address_length;
    if (make_address(path, &address, &address_length) < 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (path[0] != '@')
        unlink(path);
    if (bind(fd, (struct sockaddr *)&address, address_length) < 0 ||
        listen(fd, backlog) < 0) {
        int saved = errno;
        close(fd);
        if (path[0] != '@')
            unlink(path);
        errno = saved;
        return -1;
    }
    return fd;
}

int fma_send_message(int socket_fd, const struct fma_message *message) {
    uint8_t header[FMA_WIRE_HEADER_SIZE];
    if (fma_encode_header(message, header) < 0)
        return -1;

    struct iovec iov = {.iov_base = header, .iov_len = sizeof(header)};
    char control[CMSG_SPACE(sizeof(int) * FMA_MAX_FDS)];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (message->fd_count) {
        memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * message->fd_count);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * message->fd_count);
        memcpy(CMSG_DATA(cmsg), message->fds, sizeof(int) * message->fd_count);
    }

    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        return -1;
    size_t offset = (size_t)sent;
    while (offset < sizeof(header)) {
        sent = send(socket_fd, header + offset, sizeof(header) - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        offset += (size_t)sent;
    }
    offset = 0;
    while (offset < message->payload_size) {
        sent = send(socket_fd, message->payload + offset,
                    message->payload_size - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        offset += (size_t)sent;
    }
    return 0;
}

int fma_receive_message(int socket_fd, struct fma_message *message) {
    uint8_t header[FMA_WIRE_HEADER_SIZE];
    char control[CMSG_SPACE(sizeof(int) * FMA_MAX_FDS)];
    struct iovec iov = {.iov_base = header, .iov_len = sizeof(header)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t received;
    do {
        received = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC | MSG_WAITALL);
    } while (received < 0 && errno == EINTR);
    if (received <= 0) {
        int saved = received == 0 ? ECONNRESET : errno;
        errno = saved;
        return -1;
    }

    int received_fds[FMA_MAX_FDS];
    uint32_t actual_fds = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0))
            continue;
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        size_t count = bytes / sizeof(int);
        int *fds = (int *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < count; ++i) {
            if (actual_fds < FMA_MAX_FDS)
                received_fds[actual_fds++] = fds[i];
            else
                close(fds[i]);
        }
    }

    size_t header_bytes = (size_t)received;
    while (header_bytes < sizeof(header)) {
        received = recv(socket_fd, header + header_bytes,
                        sizeof(header) - header_bytes, MSG_WAITALL);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0) {
            for (uint32_t i = 0; i < actual_fds; ++i)
                close(received_fds[i]);
            errno = EPROTO;
            return -1;
        }
        header_bytes += (size_t)received;
    }
    if ((msg.msg_flags & MSG_CTRUNC) || fma_decode_header(header, message) < 0) {
        for (uint32_t i = 0; i < actual_fds; ++i)
            close(received_fds[i]);
        errno = EPROTO;
        return -1;
    }
    if (actual_fds != message->fd_count) {
        for (uint32_t i = 0; i < actual_fds; ++i)
            close(received_fds[i]);
        errno = EPROTO;
        return -1;
    }
    for (uint32_t i = 0; i < actual_fds; ++i) {
        message->fds[i] = received_fds[i];
#ifdef FMA_SET_RECEIVED_CLOEXEC
        (void)fcntl(message->fds[i], F_SETFD, FD_CLOEXEC);
#endif
    }

    if (message->payload_size) {
        message->payload = malloc(message->payload_size);
        if (!message->payload) {
            fma_message_release(message);
            return -1;
        }
        size_t offset = 0;
        while (offset < message->payload_size) {
            received = recv(socket_fd, message->payload + offset,
                            message->payload_size - offset, MSG_WAITALL);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0) {
                fma_message_release(message);
                errno = EPROTO;
                return -1;
            }
            offset += (size_t)received;
        }
    }
    return 0;
}
