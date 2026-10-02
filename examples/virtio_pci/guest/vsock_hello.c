#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <unistd.h>

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t n = write(fd, buffer + written, length - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, char *buffer, size_t length)
{
    size_t received = 0;

    while (received < length) {
        ssize_t n = read(fd, buffer + received, length - received);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n == 0 ? 1 : -1;
        received += (size_t)n;
    }
    return 0;
}

int main(void)
{
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("vsock-hello: socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_vm peer = {
        .svm_family = AF_VSOCK,
        .svm_cid = VMADDR_CID_HOST,
        .svm_port = 1234,
    };
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        printf("vsock-hello: connect: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("vsock-hello: connected to CID %u port %u\n",
           peer.svm_cid, peer.svm_port);
    printf("Type a line to echo over AF_VSOCK; use /quit or Ctrl-D to exit.\n");

    char line[1024];
    char response[sizeof(line)];
    for (;;) {
        fputs("vsock> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }
        if (!strcmp(line, "/quit\n") || !strcmp(line, "/quit"))
            break;

        size_t length = strlen(line);
        if (write_all(fd, line, length) < 0) {
            printf("vsock-hello: write: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        int result = read_all(fd, response, length);
        if (result != 0) {
            printf("vsock-hello: read: %s\n",
                   result > 0 ? "host closed the connection" : strerror(errno));
            close(fd);
            return 1;
        }
        fputs("echo: ", stdout);
        fwrite(response, 1, length, stdout);
        if (!length || response[length - 1] != '\n')
            putchar('\n');
    }

    close(fd);
    return 0;
}
