/*
 * Copyright 2026, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <microkit.h>
#include <stdint.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/virtio.h>
#include <libvmm/virtio/vsock_queue.h>

/*
 * https://docs.oasis-open.org/virtio/virtio/v1.2/cs01/virtio-v1.2-cs01.html#x1-4380002
 * Virtio 1.2, section 5.10.2, "Virtqueues":
 *   0. receiveq
 *   1. transmitq
 *   2. eventq
 */
#define VIRTIO_VSOCK_RX_VIRTQ    0
#define VIRTIO_VSOCK_TX_VIRTQ    1
#define VIRTIO_VSOCK_EVENT_VIRTQ 2
#define VIRTIO_VSOCK_NUM_VIRTQ   3



struct virtio_vsock_device {
    struct virtio_device virtio_device;
    struct virtio_queue_handler vqs[VIRTIO_VSOCK_NUM_VIRTQ];
    virtio_vsock_queue_handle_t *backend_queue;
    microkit_channel backend_ch;
    uint64_t guest_cid;
};

bool virtio_pci_vsock_init(struct virtio_vsock_device *vsock, uint16_t pci_bus, uint16_t pci_dev,
                           irq_routing_info_t irq_routing_info, virtio_vsock_queue_handle_t *backend_queue,
                           microkit_channel backend_ch, uint64_t guest_cid);

/* Called when the host backend signals that the backend RX queue contains packets. */
void virtio_vsock_handle_backend(struct virtio_vsock_device *vsock);
