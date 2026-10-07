/*
 * Copyright 2026, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <string.h>
#include <microkit.h>
#include <sddf/util/printf.h>
#include <libvmm/virtio/vsock_config.h>
#include <libvmm/virtio/vsock_queue.h>

#define VSOCK_ECHO_PORT 1234U
#define VSOCK_TYPE_STREAM 1U
#define VSOCK_OP_REQUEST 1U
#define VSOCK_OP_RESPONSE 2U
#define VSOCK_OP_RST 3U
#define VSOCK_OP_SHUTDOWN 4U
#define VSOCK_OP_RW 5U
#define VSOCK_OP_CREDIT_UPDATE 6U
#define VSOCK_OP_CREDIT_REQUEST 7U

struct __attribute__((packed)) vsock_hdr {
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
};

__attribute__((section(".virtio_vsock_transport_config")))
virtio_vsock_transport_config_t config;
static virtio_vsock_queue_handle_t transport;

static void reply(const struct vsock_hdr *request, uint16_t op,
                  const void *payload, uint32_t payload_len)
{
    struct vsock_hdr response = *request;
    uint8_t *packet = virtio_vsock_queue_tx_buffer(&transport);

    if (packet == NULL || sizeof(response) + payload_len > transport.buffer_size) {
        sddf_printf("VSOCK_ECHO: transmit queue full\n");
        return;
    }
    response.src_cid = request->dst_cid;
    response.dst_cid = request->src_cid;
    response.src_port = request->dst_port;
    response.dst_port = request->src_port;
    response.op = op;
    response.len = payload_len;
    response.buf_alloc = transport.capacity * transport.buffer_size;
    response.fwd_cnt = request->fwd_cnt;
    memcpy(packet, &response, sizeof(response));
    if (payload_len != 0) {
        memcpy(packet + sizeof(response), payload, payload_len);
    }
    if (virtio_vsock_queue_enqueue(&transport, sizeof(response) + payload_len) == 0) {
        microkit_notify(config.connection.id);
    }
}

static void handle_packets(void)
{
    void *packet;
    uint32_t packet_len;

    while (virtio_vsock_queue_peek(&transport, &packet, &packet_len) == 0) {
        struct vsock_hdr request;
        if (packet_len >= sizeof(request)) {
            memcpy(&request, packet, sizeof(request));
            if (request.type != VSOCK_TYPE_STREAM ||
                request.dst_cid != config.host_cid ||
                request.dst_port != VSOCK_ECHO_PORT ||
                request.len > packet_len - sizeof(request)) {
                reply(&request, VSOCK_OP_RST, NULL, 0);
            } else if (request.op == VSOCK_OP_REQUEST) {
                sddf_printf("VSOCK_ECHO: connection from CID %llu port %u\n",
                            (unsigned long long)request.src_cid, request.src_port);
                reply(&request, VSOCK_OP_RESPONSE, NULL, 0);
            } else if (request.op == VSOCK_OP_RW) {
                reply(&request, VSOCK_OP_RW,
                      (uint8_t *)packet + sizeof(request), request.len);
            } else if (request.op == VSOCK_OP_CREDIT_REQUEST) {
                reply(&request, VSOCK_OP_CREDIT_UPDATE, NULL, 0);
            } else if (request.op == VSOCK_OP_SHUTDOWN) {
                reply(&request, VSOCK_OP_RST, NULL, 0);
            }
        }
        (void)virtio_vsock_queue_dequeue(&transport);
    }
}

void init(void)
{
    virtio_vsock_connection_resource_t *c = &config.connection;
    if (!virtio_vsock_transport_config_check_magic(&config)) {
        sddf_printf("VSOCK_ECHO: invalid transport config\n");
        return;
    }
    virtio_vsock_queue_init(&transport, c->tx_queue.vaddr, c->tx_data.vaddr,
                            c->rx_queue.vaddr, c->rx_data.vaddr,
                            c->capacity, c->buffer_size);
    sddf_printf("VSOCK_ECHO: listening on CID %llu port %u\n",
                (unsigned long long)config.host_cid, VSOCK_ECHO_PORT);
}

void notified(microkit_channel ch)
{
    if (ch == config.connection.id) {
        handle_packets();
    }
}
