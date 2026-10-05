/* SPDX-License-Identifier: BSD-2-Clause */
#include "runtime_protocol.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uk/vsock.h>

struct __attribute__((packed)) rpc_header {
	uint32_t magic; uint16_t version, op; uint32_t request_id, status, length;
};

static int transfer(int fd, void *buffer, size_t length, int writing)
{
	size_t done = 0;
	while (done < length) {
		ssize_t n = writing ? write(fd, (uint8_t *)buffer + done, length - done)
				    : read(fd, (uint8_t *)buffer + done, length - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		done += (size_t)n;
	}
	return 0;
}

static int serve(int fd)
{
	struct rpc_header h;
	struct cri_object request, response;
	for (;;) {
		if (transfer(fd, &h, sizeof(h), 0)) return -1;
		if (h.magic != CRI_MAGIC || h.version != CRI_VERSION ||
		    h.length != sizeof(request)) return -1;
		if (transfer(fd, &request, sizeof(request), 0)) return -1;
		h.status = cri_dispatch(h.op, &request, &response);
		h.length = sizeof(response);
		if (transfer(fd, &h, sizeof(h), 1) || transfer(fd, &response, sizeof(response), 1)) return -1;
	}
}

int uk_app_main(void)
{
	struct sockaddr_vm local = { .svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_HOST, .svm_port = CRI_PORT };
	int listener = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (listener < 0 || bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(listener, 1) < 0) {
		printf("HOST_RUNTIME: socket/bind/listen failed: %s\n", strerror(errno)); return 1;
	}
	printf("HOST_RUNTIME: dummy native runtime listening on CID 2 port %u\n", CRI_PORT);
	for (;;) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) { printf("HOST_RUNTIME: accept failed: %s\n", strerror(errno)); continue; }
		printf("HOST_RUNTIME: guest CRI shim connected\n"); serve(client); close(client);
		printf("HOST_RUNTIME: guest CRI shim disconnected\n");
	}
}
