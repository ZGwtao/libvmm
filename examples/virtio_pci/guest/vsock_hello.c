#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <unistd.h>

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
        return 1;
    }
    static const char request[] = "hello from guest";
    if (write(fd, request, sizeof(request) - 1) != sizeof(request) - 1) {
        printf("vsock-hello: write: %s\n", strerror(errno));
        return 1;
    }
    char response[128];
    ssize_t n = read(fd, response, sizeof(response) - 1);
    if (n <= 0) {
        printf("vsock-hello: read: %s\n", n < 0 ? strerror(errno) : "EOF");
        return 1;
    }
    response[n] = '\0';
    printf("vsock-hello: received: %s", response);
    close(fd);
    return 0;
}
