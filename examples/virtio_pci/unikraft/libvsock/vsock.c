/* SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <carrels/events.h>
#include <microkit.h>
#include <uk/alloc.h>
#include <uk/errptr.h>
#include <uk/file.h>
#include <uk/print.h>
#include <uk/socket_driver.h>
#include <uk/vsock.h>

#include <libvmm/virtio/vsock_config.h>
#include <libvmm/virtio/vsock_queue.h>

#define VSOCK_HOST_CID 2U
#define VSOCK_TYPE_STREAM 1U
#define VSOCK_OP_REQUEST 1U
#define VSOCK_OP_RESPONSE 2U
#define VSOCK_OP_RST 3U
#define VSOCK_OP_SHUTDOWN 4U
#define VSOCK_OP_RW 5U
#define VSOCK_OP_CREDIT_UPDATE 6U
#define VSOCK_OP_CREDIT_REQUEST 7U
#define VSOCK_RX_CAPACITY 4096U

struct vsock_hdr {
	uint64_t src_cid;
	uint64_t dst_cid;
	uint32_t src_port;
	uint32_t dst_port;
	uint32_t len;
	uint16_t type;
	uint16_t op;
	uint32_t flags;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} __attribute__((packed));

enum socket_state {
	VSOCK_CREATED,
	VSOCK_LISTENING,
	VSOCK_CONNECTED,
	VSOCK_CLOSED,
};

struct vsock_socket {
	enum socket_state state;
	posix_sock *file;
	uint32_t local_port;
	uint64_t peer_cid;
	uint32_t peer_port;
	uint32_t peer_buf_alloc;
	uint32_t peer_fwd_cnt;
	uint32_t tx_count;
	uint32_t rx_fwd_cnt;
	uint8_t rx[VSOCK_RX_CAPACITY];
	uint32_t rx_len;
};

__attribute__((section(".virtio_vsock_transport_config")))
virtio_vsock_transport_config_t vsock_config;

static virtio_vsock_queue_handle_t transport;
static struct posix_socket_driver *driver;
static struct vsock_socket *listener;
static struct {
	bool valid;
	uint64_t cid;
	uint32_t port;
	uint32_t dst_port;
	uint32_t buf_alloc;
	uint32_t fwd_cnt;
} pending;
static struct vsock_socket *connection;

static void notify_peer(void)
{
	microkit_notify(vsock_config.connection.id);
}

static bool send_packet(struct vsock_socket *sock, uint16_t op,
			const void *payload, uint32_t len)
{
	void *packet = virtio_vsock_queue_tx_buffer(&transport);
	struct vsock_hdr hdr;

	if (!packet || sizeof(hdr) + len > transport.buffer_size)
		return false;
	hdr = (struct vsock_hdr) {
		.src_cid = VSOCK_HOST_CID,
		.dst_cid = sock->peer_cid,
		.src_port = sock->local_port,
		.dst_port = sock->peer_port,
		.len = len,
		.type = VSOCK_TYPE_STREAM,
		.op = op,
		.buf_alloc = VSOCK_RX_CAPACITY,
		.fwd_cnt = sock->rx_fwd_cnt,
	};
	memcpy(packet, &hdr, sizeof(hdr));
	if (len)
		memcpy((uint8_t *)packet + sizeof(hdr), payload, len);
	if (virtio_vsock_queue_enqueue(&transport, sizeof(hdr) + len))
		return false;
	if (op == VSOCK_OP_RW)
		sock->tx_count += len;
	notify_peer();
	return true;
}

static void reject(const struct vsock_hdr *request)
{
	struct vsock_socket tmp = {
		.local_port = request->dst_port,
		.peer_cid = request->src_cid,
		.peer_port = request->src_port,
	};
	(void)send_packet(&tmp, VSOCK_OP_RST, NULL, 0);
}

static void process_packet(const void *packet, uint32_t packet_len)
{
	struct vsock_hdr hdr;

	if (packet_len < sizeof(hdr))
		return;
	memcpy(&hdr, packet, sizeof(hdr));
	if (hdr.len > packet_len - sizeof(hdr) ||
	    hdr.dst_cid != VSOCK_HOST_CID || hdr.type != VSOCK_TYPE_STREAM)
		return;

	if (hdr.op == VSOCK_OP_REQUEST) {
		if (!listener || listener->state != VSOCK_LISTENING ||
		    listener->local_port != hdr.dst_port || pending.valid || connection) {
			reject(&hdr);
			return;
		}
		pending.valid = true;
		pending.cid = hdr.src_cid;
		pending.port = hdr.src_port;
		pending.dst_port = hdr.dst_port;
		pending.buf_alloc = hdr.buf_alloc;
		pending.fwd_cnt = hdr.fwd_cnt;
		if (listener->file)
			posix_sock_event_set(listener->file, UKFD_POLLIN);
		return;
	}

	if (!connection || connection->peer_cid != hdr.src_cid ||
	    connection->peer_port != hdr.src_port ||
	    connection->local_port != hdr.dst_port) {
		if (hdr.op != VSOCK_OP_RST)
			reject(&hdr);
		return;
	}

	connection->peer_buf_alloc = hdr.buf_alloc;
	connection->peer_fwd_cnt = hdr.fwd_cnt;
	switch (hdr.op) {
	case VSOCK_OP_RW:
		if (hdr.len <= VSOCK_RX_CAPACITY - connection->rx_len) {
			memcpy(connection->rx + connection->rx_len,
			       (const uint8_t *)packet + sizeof(hdr), hdr.len);
			connection->rx_len += hdr.len;
			if (connection->file)
				posix_sock_event_set(connection->file, UKFD_POLLIN);
		}
		break;
	case VSOCK_OP_CREDIT_REQUEST:
		(void)send_packet(connection, VSOCK_OP_CREDIT_UPDATE, NULL, 0);
		break;
	case VSOCK_OP_CREDIT_UPDATE:
		if (connection->file)
			posix_sock_event_set(connection->file, UKFD_POLLOUT);
		break;
	case VSOCK_OP_SHUTDOWN:
		(void)send_packet(connection, VSOCK_OP_RST, NULL, 0);
		connection->state = VSOCK_CLOSED;
		if (connection->file)
			posix_sock_event_set(connection->file, UKFD_POLLIN | EPOLLHUP);
		break;
	case VSOCK_OP_RST:
		connection->state = VSOCK_CLOSED;
		if (connection->file)
			posix_sock_event_set(connection->file, UKFD_POLLIN | EPOLLHUP);
		break;
	default:
		break;
	}
}

static void transport_event(microkit_channel ch __attribute__((unused)),
			    void *arg __attribute__((unused)))
{
	void *packet;
	uint32_t len;
	bool consumed = false;

	while (!virtio_vsock_queue_peek(&transport, &packet, &len)) {
		process_packet(packet, len);
		(void)virtio_vsock_queue_dequeue(&transport);
		consumed = true;
	}
	if (consumed)
		notify_peer();
}

static int vsock_init(struct posix_socket_driver *d)
{
	virtio_vsock_connection_resource_t *c = &vsock_config.connection;

	if (!virtio_vsock_transport_config_check_magic(&vsock_config))
		return -EINVAL;
	driver = d;
	virtio_vsock_queue_init(&transport, c->tx_queue.vaddr, c->tx_data.vaddr,
				c->rx_queue.vaddr, c->rx_data.vaddr,
				c->capacity, c->buffer_size);
	if (carrels_event_register(c->id, transport_event, NULL))
		return -EBUSY;
	uk_pr_info("VSOCK: host AF_VSOCK transport ready on CID 2\n");
	return 0;
}

static void *vsock_create(struct posix_socket_driver *d, int family,
			  int type, int protocol)
{
	struct vsock_socket *sock;

	if (d != driver || family != AF_VSOCK || (type & ~SOCK_FLAGS) != SOCK_STREAM ||
	    protocol != 0)
		return ERR2PTR(-EPROTONOSUPPORT);
	sock = uk_calloc(d->allocator, 1, sizeof(*sock));
	if (!sock)
		return ERR2PTR(-ENOMEM);
	sock->state = VSOCK_CREATED;
	return sock;
}

static int vsock_bind(posix_sock *file, const struct sockaddr *addr,
		      socklen_t addr_len)
{
	struct vsock_socket *sock = posix_sock_get_data(file);
	const struct sockaddr_vm *vm = (const struct sockaddr_vm *)addr;

	if (!addr || addr_len < sizeof(*vm) || vm->svm_family != AF_VSOCK ||
	    (vm->svm_cid != VMADDR_CID_ANY && vm->svm_cid != VSOCK_HOST_CID) ||
	    !vm->svm_port)
		return -EINVAL;
	if (sock->state != VSOCK_CREATED)
		return -EINVAL;
	sock->local_port = vm->svm_port;
	return 0;
}

static int vsock_listen(posix_sock *file, int backlog __attribute__((unused)))
{
	struct vsock_socket *sock = posix_sock_get_data(file);

	if (!sock->local_port || (listener && listener != sock))
		return -EINVAL;
	sock->state = VSOCK_LISTENING;
	sock->file = file;
	listener = sock;
	return 0;
}

static void *vsock_accept4(posix_sock *file, struct sockaddr *addr,
			   socklen_t *addr_len, int flags __attribute__((unused)))
{
	struct vsock_socket *sock;
	struct sockaddr_vm peer;

	if (posix_sock_get_data(file) != listener || !pending.valid)
		return ERR2PTR(-EAGAIN);
	sock = uk_calloc(driver->allocator, 1, sizeof(*sock));
	if (!sock)
		return ERR2PTR(-ENOMEM);
	sock->state = VSOCK_CONNECTED;
	sock->local_port = pending.dst_port;
	sock->peer_cid = pending.cid;
	sock->peer_port = pending.port;
	sock->peer_buf_alloc = pending.buf_alloc;
	sock->peer_fwd_cnt = pending.fwd_cnt;
	connection = sock;
	pending.valid = false;
	posix_sock_event_clear(file, UKFD_POLLIN);

	peer = (struct sockaddr_vm) {
		.svm_family = AF_VSOCK,
		.svm_cid = (uint32_t)sock->peer_cid,
		.svm_port = sock->peer_port,
	};
	if (addr && addr_len) {
		socklen_t n = *addr_len < sizeof(peer) ? *addr_len : sizeof(peer);
		memcpy(addr, &peer, n);
		*addr_len = sizeof(peer);
	}
	if (!send_packet(sock, VSOCK_OP_RESPONSE, NULL, 0)) {
		connection = NULL;
		uk_free(driver->allocator, sock);
		return ERR2PTR(-EAGAIN);
	}
	return sock;
}

static ssize_t vsock_read(posix_sock *file, const struct iovec *iov,
			  size_t iovcnt)
{
	struct vsock_socket *sock = posix_sock_get_data(file);
	size_t done = 0;

	if (!sock->rx_len)
		return sock->state == VSOCK_CLOSED ? 0 : -EAGAIN;
	for (size_t i = 0; i < iovcnt && done < sock->rx_len; i++) {
		size_t n = iov[i].iov_len;
		if (n > sock->rx_len - done)
			n = sock->rx_len - done;
		memcpy(iov[i].iov_base, sock->rx + done, n);
		done += n;
	}
	memmove(sock->rx, sock->rx + done, sock->rx_len - done);
	sock->rx_len -= done;
	sock->rx_fwd_cnt += done;
	if (!sock->rx_len)
		posix_sock_event_clear(file, UKFD_POLLIN);
	(void)send_packet(sock, VSOCK_OP_CREDIT_UPDATE, NULL, 0);
	return done;
}

static ssize_t vsock_write(posix_sock *file, const struct iovec *iov,
			   size_t iovcnt)
{
	struct vsock_socket *sock = posix_sock_get_data(file);
	uint8_t payload[VSOCK_RX_CAPACITY];
	size_t total = 0;
	uint32_t peer_used = sock->tx_count - sock->peer_fwd_cnt;
	uint32_t credit = sock->peer_buf_alloc - peer_used;

	if (sock->state != VSOCK_CONNECTED)
		return -ENOTCONN;
	for (size_t i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len > sizeof(payload) - total)
			return total ? (ssize_t)total : -EMSGSIZE;
		memcpy(payload + total, iov[i].iov_base, iov[i].iov_len);
		total += iov[i].iov_len;
	}
	if (total > credit || !send_packet(sock, VSOCK_OP_RW, payload, total)) {
		posix_sock_event_clear(file, UKFD_POLLOUT);
		return -EAGAIN;
	}
	return total;
}

static int vsock_close(posix_sock *file)
{
	struct vsock_socket *sock = posix_sock_get_data(file);

	if (sock == listener) {
		listener = NULL;
		pending.valid = false;
	} else if (sock == connection) {
		if (sock->state == VSOCK_CONNECTED)
			(void)send_packet(sock, VSOCK_OP_SHUTDOWN, NULL, 0);
		connection = NULL;
	}
	uk_free(driver->allocator, sock);
	return 0;
}

static int vsock_ioctl(posix_sock *file __attribute__((unused)),
		       int request __attribute__((unused)),
		       void *argp __attribute__((unused)))
{
	return -ENOSYS;
}

static void vsock_poll_setup(posix_sock *file)
{
	struct vsock_socket *sock = posix_sock_get_data(file);

	sock->file = file;
	if (sock->state == VSOCK_CONNECTED)
		posix_sock_event_set(file, UKFD_POLLOUT);
}

static const struct posix_socket_ops vsock_ops = {
	.init = vsock_init,
	.create = vsock_create,
	.accept4 = vsock_accept4,
	.bind = vsock_bind,
	.listen = vsock_listen,
	.read = vsock_read,
	.write = vsock_write,
	.close = vsock_close,
	.ioctl = vsock_ioctl,
	.poll_setup = vsock_poll_setup,
};

POSIX_SOCKET_FAMILY_REGISTER(AF_VSOCK, &vsock_ops);
