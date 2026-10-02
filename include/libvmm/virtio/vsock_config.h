/*
 * Copyright 2026, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sddf/resources/common.h>

#define VIRTIO_VSOCK_TRANSPORT_MAGIC_LEN 5
static const char VIRTIO_VSOCK_TRANSPORT_MAGIC[VIRTIO_VSOCK_TRANSPORT_MAGIC_LEN] = {
    's', 'D', 'D', 'F', 0x7
};

typedef struct virtio_vsock_connection_resource {
    region_resource_t tx_queue;
    region_resource_t tx_data;
    region_resource_t rx_queue;
    region_resource_t rx_data;
    uint32_t capacity;
    uint32_t buffer_size;
    uint8_t id;
} virtio_vsock_connection_resource_t;

typedef struct virtio_vsock_transport_config {
    char magic[VIRTIO_VSOCK_TRANSPORT_MAGIC_LEN];
    virtio_vsock_connection_resource_t connection;
} virtio_vsock_transport_config_t;

/* meta.py serialises this ABI directly for 64-bit Microkit systems. */
_Static_assert(offsetof(virtio_vsock_transport_config_t, connection) == 8, "unexpected config padding");
_Static_assert(sizeof(virtio_vsock_connection_resource_t) == 80, "unexpected connection config size");
_Static_assert(sizeof(virtio_vsock_transport_config_t) == 88, "unexpected transport config size");

static inline bool virtio_vsock_transport_config_check_magic(const void *config)
{
    const char *magic = config;
    for (size_t i = 0; i < VIRTIO_VSOCK_TRANSPORT_MAGIC_LEN; i++) {
        if (magic[i] != VIRTIO_VSOCK_TRANSPORT_MAGIC[i]) {
            return false;
        }
    }
    return true;
}
