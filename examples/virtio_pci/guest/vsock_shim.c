#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <sys/un.h>
#include <unistd.h>

#define SHIM_SOCKET_PATH "/run/vsock-shim.sock"
#define HOST_PORT 1234U
#define MAX_MESSAGE_SIZE 1024U

static int transfer_all(int fd, void *buffer, size_t length, int send_data)
{
    size_t done = 0;

    while (done < length) {
        ssize_t n = send_data
            ? send(fd, (char *)buffer + done, length - done, MSG_NOSIGNAL)
            : read(fd, (char *)buffer + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n == 0 ? 1 : -1;
        done += (size_t)n;
    }
    return 0;
}

static uint32_t decode_length(const uint8_t frame[4])
{
    return ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
           ((uint32_t)frame[2] << 8) | frame[3];
}

static int connect_host(void)
{
    struct sockaddr_vm peer = {
        .svm_family = AF_VSOCK,
        .svm_cid = VMADDR_CID_HOST,
        .svm_port = HOST_PORT,
    };
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);

    if (fd >= 0 && connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static void proxy_client(int local_fd)
{
    uint8_t frame[4];
    char message[MAX_MESSAGE_SIZE];
    int host_fd = connect_host();

    if (host_fd < 0) {
        printf("vsock-shim: host connect failed: %s\n", strerror(errno));
        return;
    }

    for (;;) {
        int result = transfer_all(local_fd, frame, sizeof(frame), 0);
        if (result != 0)
            break;

        uint32_t length = decode_length(frame);
        if (!length || length > sizeof(message)) {
            printf("vsock-shim: invalid message length %u\n", length);
            break;
        }
        if (transfer_all(local_fd, message, length, 0) != 0 ||
            transfer_all(host_fd, message, length, 1) != 0 ||
            transfer_all(host_fd, message, length, 0) != 0 ||
            transfer_all(local_fd, frame, sizeof(frame), 1) != 0 ||
            transfer_all(local_fd, message, length, 1) != 0) {
            printf("vsock-shim: forwarding failed: %s\n", strerror(errno));
            break;
        }
    }

    close(host_fd);
}

int main(void)
{
    struct sockaddr_un local = {
        .sun_family = AF_UNIX,
        .sun_path = SHIM_SOCKET_PATH,
    };
    int listener;

    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        printf("vsock-shim: socket: %s\n", strerror(errno));
        return 1;
    }
    unlink(SHIM_SOCKET_PATH);
    if (bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        listen(listener, 1) < 0) {
        printf("vsock-shim: bind/listen: %s\n", strerror(errno));
        close(listener);
        return 1;
    }

    printf("vsock-shim: UDS %s -> CID %u port %u\n",
           SHIM_SOCKET_PATH, VMADDR_CID_HOST, HOST_PORT);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno != EINTR)
                printf("vsock-shim: accept: %s\n", strerror(errno));
            continue;
        }
        proxy_client(client);
        close(client);
    }
}
