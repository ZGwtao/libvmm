/*
 * Copyright 2026, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <libvmm/pci.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/vsock.h>

#define LOG_VSOCK_ERR(...) do { printf("VIRTIO(VSOCK)|ERROR: "); printf(__VA_ARGS__); } while (0)

static inline struct virtio_vsock_device *state(struct virtio_device *dev)
{
    return dev->device_data;
}

static void regs_init(struct virtio_device *dev)
{
    dev->regs.device_id = VIRTIO_DEVICE_ID_VSOCK;
    dev->regs.vendor_id = VIRTIO_DEV_VENDOR_ID;
}

static void device_reset(struct virtio_device *dev)
{
    for (size_t i = 0; i < dev->num_vqs; i++) {
        memset(&dev->vqs[i], 0, sizeof(dev->vqs[i]));
    }
    virtio_set_interrupt_status(dev, false, false);
    memset(&dev->regs, 0, sizeof(dev->regs));
    regs_init(dev);
}

static bool get_device_features(struct virtio_device *dev, uint32_t *features)
{
    *features = dev->regs.device_features_sel == 1 ? BIT_HIGH(VIRTIO_F_VERSION_1) : 0;
    return true;
}

static bool set_driver_features(struct virtio_device *dev, uint32_t features)
{
    bool valid = dev->regs.driver_features_sel == 0 ? features == 0
                                                    : features == BIT_HIGH(VIRTIO_F_VERSION_1);
    if (valid) {
        dev->regs.driver_features = features;
        dev->features_happy = true;
    }
    return valid;
}

static bool get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *value)
{
    uint64_t cid = state(dev)->guest_cid;
    if (offset == 0) {
        *value = (uint32_t)cid;
    } else if (offset == 4) {
        *value = (uint32_t)(cid >> 32);
    } else {
        return false;
    }
    return true;
}

static bool set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t value)
{
    (void)dev;
    (void)offset;
    (void)value;
    return false;
}

static bool respond(struct virtio_device *dev)
{
    virtio_set_interrupt_status(dev, true, false);
    return virtio_inject_interrupt(dev);
}

static bool flush_tx(struct virtio_device *dev)
{
    struct virtio_vsock_device *vsock = state(dev);
    virtio_vsock_queue_handle_t *queue = vsock->backend_queue;
    struct virtio_queue_handler *vq = &dev->vqs[VIRTIO_VSOCK_TX_VIRTQ];
    uint16_t head;
    bool used = false;
    bool produced = false;

    if (!vq->ready) {
        return true;
    }
    while (!virtio_vsock_queue_full_tx(queue) && virtio_virtq_peek_avail(vq, &head)) {
        uint64_t len = virtio_desc_chain_payload_len(vq, head);
        if (len == 0 || len > queue->buffer_size) {
            LOG_VSOCK_ERR("invalid TX packet length %lu\n", len);
            (void)virtio_virtq_pop_avail(vq, &head);
            virtio_virtq_add_used(vq, head, 0);
            used = true;
            continue;
        }

        void *packet = virtio_vsock_queue_tx_buffer(queue);
        if (!packet || !virtio_read_data_from_desc_chain(vq, head, len, 0, packet)) {
            return false;
        }
        (void)virtio_virtq_pop_avail(vq, &head);
        virtio_virtq_add_used(vq, head, 0);
        if (virtio_vsock_queue_enqueue(queue, len)) {
            return false;
        }
        produced = true;
        used = true;
    }
    if (produced) {
        microkit_notify(vsock->backend_ch);
    }
    return !used || respond(dev);
}

static bool flush_rx(struct virtio_device *dev)
{
    struct virtio_vsock_device *vsock = state(dev);
    virtio_vsock_queue_handle_t *queue = vsock->backend_queue;
    struct virtio_queue_handler *vq = &dev->vqs[VIRTIO_VSOCK_RX_VIRTQ];
    uint16_t head;
    bool used = false;

    if (!vq->ready) {
        return true;
    }
    while (!virtio_vsock_queue_empty_rx(queue) && virtio_virtq_peek_avail(vq, &head)) {
        void *packet;
        uint32_t packet_len;
        if (virtio_vsock_queue_peek(queue, &packet, &packet_len)) {
            return false;
        }
        uint64_t capacity = virtio_desc_chain_payload_len(vq, head);
        if (packet_len > capacity || packet_len > queue->buffer_size) {
            LOG_VSOCK_ERR("RX packet %u exceeds guest buffer %lu\n", packet_len, capacity);
            return false;
        }
        if (!virtio_write_data_to_desc_chain(vq, head, packet_len, 0, packet)) {
            return false;
        }
        (void)virtio_virtq_pop_avail(vq, &head);
        virtio_virtq_add_used(vq, head, packet_len);
        if (virtio_vsock_queue_dequeue(queue)) {
            return false;
        }
        used = true;
    }
    return !used || respond(dev);
}

static bool queue_notify(struct virtio_device *dev)
{
    switch (dev->regs.queue_notify) {
    case VIRTIO_VSOCK_RX_VIRTQ:
        return flush_rx(dev);
    case VIRTIO_VSOCK_TX_VIRTQ:
        return flush_tx(dev);
    case VIRTIO_VSOCK_EVENT_VIRTQ:
        return true;
    default:
        return false;
    }
}

static virtio_device_funs_t functions = {
    .device_reset = device_reset,
    .get_device_features = get_device_features,
    .set_driver_features = set_driver_features,
    .get_device_config = get_device_config,
    .set_device_config = set_device_config,
    .queue_notify = queue_notify,
};

bool virtio_pci_vsock_init(struct virtio_vsock_device *vsock, uint16_t pci_bus, uint16_t pci_dev,
                           irq_routing_info_t irq_routing_info, virtio_vsock_queue_handle_t *backend_queue,
                           microkit_channel backend_ch, uint64_t guest_cid)
{
    memset(vsock, 0, sizeof(*vsock));
    struct virtio_device *dev = &vsock->virtio_device;
    regs_init(dev);
    dev->transport_type = VIRTIO_TRANSPORT_PCI;
    dev->funs = &functions;
    dev->vqs = vsock->vqs;
    dev->num_vqs = VIRTIO_VSOCK_NUM_VIRTQ;
    dev->irq_routing_info = irq_routing_info;
    dev->device_data = vsock;
    vsock->backend_queue = backend_queue;
    vsock->backend_ch = backend_ch;
    vsock->guest_cid = guest_cid;
    dev->transport.pci.device_id = VIRTIO_PCI_MODERN_BASE_DEVICE_ID + VIRTIO_DEVICE_ID_VSOCK;
    dev->transport.pci.vendor_id = VIRTIO_PCI_VENDOR_ID;
    dev->transport.pci.device_class = PCI_CLASS_COMMUNICATION_OTHER;
    return virtio_pci_register_device(dev, pci_bus, pci_dev, irq_routing_info);
}

void virtio_vsock_handle_backend(struct virtio_vsock_device *vsock)
{
    if (!flush_rx(&vsock->virtio_device)) {
        LOG_VSOCK_ERR("failed to deliver backend packet\n");
    }
    /* Consumption by the backend may have released TX backpressure. */
    if (!flush_tx(&vsock->virtio_device)) {
        LOG_VSOCK_ERR("failed to resume guest TX\n");
    }
}
