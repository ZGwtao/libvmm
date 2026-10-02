#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SHIM_SOCKET_PATH "/run/vsock-shim.sock"
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

static void encode_length(uint8_t frame[4], uint32_t length)
{
    frame[0] = (uint8_t)(length >> 24);
    frame[1] = (uint8_t)(length >> 16);
    frame[2] = (uint8_t)(length >> 8);
    frame[3] = (uint8_t)length;
}

static uint32_t decode_length(const uint8_t frame[4])
{
    return ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
           ((uint32_t)frame[2] << 8) | frame[3];
}

int main(void)
{
    struct sockaddr_un peer = {
        .sun_family = AF_UNIX,
        .sun_path = SHIM_SOCKET_PATH,
    };
    char line[MAX_MESSAGE_SIZE];
    char response[MAX_MESSAGE_SIZE];
    uint8_t frame[4];
    int fd;

    signal(SIGPIPE, SIG_IGN);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        printf("uds-client: connect %s: %s\n", SHIM_SOCKET_PATH,
               strerror(errno));
        if (fd >= 0)
            close(fd);
        return 1;
    }

    printf("uds-client: connected to %s\n", SHIM_SOCKET_PATH);
    printf("Type a line to echo through UDS and AF_VSOCK; use /quit or Ctrl-D to exit.\n");
    for (;;) {
        fputs("uds> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            putchar('\n');
            break;
        }
        if (!strcmp(line, "/quit\n") || !strcmp(line, "/quit"))
            break;

        uint32_t length = (uint32_t)strlen(line);
        encode_length(frame, length);
        if (transfer_all(fd, frame, sizeof(frame), 1) != 0 ||
            transfer_all(fd, line, length, 1) != 0 ||
            transfer_all(fd, frame, sizeof(frame), 0) != 0) {
            printf("uds-client: shim connection failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        uint32_t response_length = decode_length(frame);
        if (!response_length || response_length > sizeof(response) ||
            transfer_all(fd, response, response_length, 0) != 0) {
            printf("uds-client: invalid or incomplete response\n");
            close(fd);
            return 1;
        }
        fputs("echo: ", stdout);
        fwrite(response, 1, response_length, stdout);
        if (response[response_length - 1] != '\n')
            putchar('\n');
    }

    close(fd);
    return 0;
}
