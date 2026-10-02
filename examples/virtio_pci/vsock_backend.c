/* Minimal stream-only host backend used by this example. */
#include <microkit.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sddf/util/printf.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/vsock_config.h>
#include <libvmm/virtio/vsock.h>

#define VSOCK_HOST_CID     2
#define VSOCK_LISTEN_PORT  1234
#define VSOCK_TYPE_STREAM  1
#define VSOCK_OP_REQUEST   1
#define VSOCK_OP_RESPONSE  2
#define VSOCK_OP_RST       3
#define VSOCK_OP_SHUTDOWN  4
#define VSOCK_OP_RW        5
#define VSOCK_OP_CREDIT_UPDATE  6
#define VSOCK_OP_CREDIT_REQUEST 7

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

__attribute__((__section__(".virtio_vsock_transport_config"))) virtio_vsock_transport_config_t vsock_config;
static virtio_vsock_queue_handle_t vsock_queue;
static bool connected;
static uint64_t guest_cid;
static uint32_t guest_port;
static uint32_t guest_bytes_consumed;
static uint32_t host_bytes_sent;

static bool send_packet(const struct vsock_hdr *hdr, const void *payload)
{
    void *packet = virtio_vsock_queue_tx_buffer(&vsock_queue);
    if (!packet) {
        printf("VSOCK_BACKEND: output ring full\n");
        return false;
    }
    uint32_t total = sizeof(*hdr) + hdr->len;
    if (total > vsock_queue.buffer_size) {
        return false;
    }
    memcpy(packet, hdr, sizeof(*hdr));
    if (hdr->len) {
        memcpy((uint8_t *)packet + sizeof(*hdr), payload, hdr->len);
    }
    return !virtio_vsock_queue_enqueue(&vsock_queue, total);
}

static bool reply(uint16_t op, const void *payload, uint32_t len)
{
    struct vsock_hdr hdr = {
        .src_cid = VSOCK_HOST_CID,
        .dst_cid = guest_cid,
        .src_port = VSOCK_LISTEN_PORT,
        .dst_port = guest_port,
        .len = len,
        .type = VSOCK_TYPE_STREAM,
        .op = op,
        .buf_alloc = 64 * 1024,
        .fwd_cnt = guest_bytes_consumed,
    };
    if (send_packet(&hdr, payload)) {
        host_bytes_sent += len;
        return true;
    }
    return false;
}

static bool handle_packet(const void *packet, uint32_t packet_len)
{
    struct vsock_hdr hdr;
    if (packet_len < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, packet, sizeof(hdr));
    if (hdr.len > packet_len - sizeof(hdr) || hdr.type != VSOCK_TYPE_STREAM || hdr.dst_cid != VSOCK_HOST_CID) {
        return false;
    }

    if (hdr.op == VSOCK_OP_REQUEST && hdr.dst_port == VSOCK_LISTEN_PORT) {
        guest_cid = hdr.src_cid;
        guest_port = hdr.src_port;
        guest_bytes_consumed = 0;
        host_bytes_sent = 0;
        connected = true;
        printf("VSOCK_BACKEND: stream connection %lu:%u -> 2:%u\n", guest_cid, guest_port, VSOCK_LISTEN_PORT);
        return reply(VSOCK_OP_RESPONSE, NULL, 0);
    }

    if (!connected || hdr.src_cid != guest_cid || hdr.src_port != guest_port || hdr.dst_port != VSOCK_LISTEN_PORT) {
        return false;
    }
    switch (hdr.op) {
    case VSOCK_OP_RW: {
        static const char hello[] = "hello world from vsock backend\n";
        guest_bytes_consumed += hdr.len;
        printf("VSOCK_BACKEND: received %u stream bytes\n", hdr.len);
        return reply(VSOCK_OP_RW, hello, sizeof(hello) - 1);
    }
    case VSOCK_OP_CREDIT_REQUEST:
        return reply(VSOCK_OP_CREDIT_UPDATE, NULL, 0);
    case VSOCK_OP_CREDIT_UPDATE:
        return false;
    case VSOCK_OP_SHUTDOWN:
        connected = false;
        return reply(VSOCK_OP_RST, NULL, 0);
    case VSOCK_OP_RST:
        connected = false;
        return false;
    default:
        return false;
    }
}

static void process_guest_packets(void)
{
    bool produced = false;
    bool consumed = false;
    void *packet;
    uint32_t packet_len;
    while (!virtio_vsock_queue_peek(&vsock_queue, &packet, &packet_len)) {
        produced |= handle_packet(packet, packet_len);
        (void)virtio_vsock_queue_dequeue(&vsock_queue);
        consumed = true;
    }
    if (produced || consumed) {
        microkit_notify(vsock_config.connection.id);
    }
}

void init(void)
{
    assert(virtio_vsock_transport_config_check_magic(&vsock_config));
    virtio_vsock_connection_resource_t *vsock = &vsock_config.connection;
    virtio_vsock_queue_init(&vsock_queue, vsock->tx_queue.vaddr, vsock->tx_data.vaddr,
                            vsock->rx_queue.vaddr, vsock->rx_data.vaddr,
                            vsock->capacity, vsock->buffer_size);
    printf("VSOCK_BACKEND: listening on CID 2 port %u\n", VSOCK_LISTEN_PORT);
}

void notified(microkit_channel ch)
{
    if (ch == vsock_config.connection.id) {
        process_guest_packets();
    }
}
