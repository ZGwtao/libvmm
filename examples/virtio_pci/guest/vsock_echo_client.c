/* Copyright 2026, UNSW. SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int transfer(int fd, void *buffer, size_t length, int writing)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = writing ? write(fd, (char *)buffer + done, length - done)
                            : read(fd, (char *)buffer + done, length - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int main(void)
{
    static const char message[] = "hello over virtio-vsock";
    char response[sizeof(message)] = { 0 };
    struct sockaddr_vm peer = {
        .svm_family = AF_VSOCK,
        .svm_cid = VMADDR_CID_HOST,
        .svm_port = 1234,
    };
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        printf("vsock echo: connect failed: %s\n", strerror(errno));
        return 1;
    }
    if (transfer(fd, (void *)message, sizeof(message), 1) != 0 ||
        transfer(fd, response, sizeof(response), 0) != 0) {
        printf("vsock echo: transfer failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    if (memcmp(message, response, sizeof(message)) != 0) {
        printf("vsock echo: response mismatch\n");
        return 1;
    }
    printf("vsock echo: received '%s'\n", response);
    return 0;
}
