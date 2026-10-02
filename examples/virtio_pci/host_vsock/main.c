/* SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <uk/vsock.h>

#define HELLO_PORT 1234

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

int uk_app_main(void)
{
	int listener;
	struct sockaddr_vm local = {
		.svm_family = AF_VSOCK,
		.svm_cid = VMADDR_CID_HOST,
		.svm_port = HELLO_PORT,
	};
	char request[1024];

	listener = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (listener < 0) {
		printf("HOST_VSOCK_APP: socket failed: %s\n", strerror(errno));
		return 1;
	}
	if (bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    listen(listener, 1) < 0) {
		printf("HOST_VSOCK_APP: bind/listen failed: %s\n", strerror(errno));
		return 1;
	}

	printf("HOST_VSOCK_APP: listening on CID 2 port %u\n", HELLO_PORT);
	for (;;) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			printf("HOST_VSOCK_APP: accept failed: %s\n", strerror(errno));
			continue;
		}
		printf("HOST_VSOCK_APP: client connected\n");

		for (;;) {
			ssize_t n = read(client, request, sizeof(request));
			if (n == 0) {
				printf("HOST_VSOCK_APP: client disconnected\n");
				break;
			}
			if (n < 0) {
				if (errno == EINTR)
					continue;
				printf("HOST_VSOCK_APP: read failed: %s\n",
				       strerror(errno));
				break;
			}

			printf("HOST_VSOCK_APP: echoing %zd byte(s): ", n);
			printf("%.*s", (int)n, request);
			if (request[n - 1] != '\n')
				putchar('\n');
			if (write_all(client, request, (size_t)n) < 0) {
				printf("HOST_VSOCK_APP: write failed: %s\n",
				       strerror(errno));
				break;
			}
		}
		close(client);
	}
}
