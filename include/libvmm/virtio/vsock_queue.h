/*
 * Copyright 2026, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sddf/util/fence.h>

/* Metadata for one complete virtio-vsock packet in the associated data region. */
typedef struct virtio_vsock_packet {
    uint32_t len;
} virtio_vsock_packet_t;

/* A single-producer, single-consumer packet queue. */
typedef struct virtio_vsock_queue {
    uint32_t tail;
    uint32_t head;
    virtio_vsock_packet_t packets[];
} virtio_vsock_queue_t;

/* Endpoint-relative handles: tx is produced locally and rx is consumed locally. */
typedef struct virtio_vsock_queue_handle {
    virtio_vsock_queue_t *tx;
    virtio_vsock_queue_t *rx;
    uint8_t *tx_data;
    uint8_t *rx_data;
    uint32_t capacity;
    uint32_t buffer_size;
} virtio_vsock_queue_handle_t;

static inline void virtio_vsock_queue_init(virtio_vsock_queue_handle_t *handle,
                                            virtio_vsock_queue_t *tx, void *tx_data,
                                            virtio_vsock_queue_t *rx, void *rx_data,
                                            uint32_t capacity, uint32_t buffer_size)
{
    handle->tx = tx;
    handle->rx = rx;
    handle->tx_data = tx_data;
    handle->rx_data = rx_data;
    handle->capacity = capacity;
    handle->buffer_size = buffer_size;
}

static inline bool virtio_vsock_queue_full_tx(virtio_vsock_queue_handle_t *handle)
{
    uint32_t head = load_acquire_32(&handle->tx->head);
    return handle->tx->tail - head == handle->capacity;
}

static inline bool virtio_vsock_queue_empty_rx(virtio_vsock_queue_handle_t *handle)
{
    uint32_t tail = load_acquire_32(&handle->rx->tail);
    return handle->rx->head == tail;
}

static inline void *virtio_vsock_queue_tx_buffer(virtio_vsock_queue_handle_t *handle)
{
    if (virtio_vsock_queue_full_tx(handle)) {
        return NULL;
    }
    return handle->tx_data + (handle->tx->tail % handle->capacity) * handle->buffer_size;
}

static inline int virtio_vsock_queue_enqueue(virtio_vsock_queue_handle_t *handle, uint32_t len)
{
    if (len > handle->buffer_size || virtio_vsock_queue_full_tx(handle)) {
        return -1;
    }

    uint32_t tail = handle->tx->tail;
    handle->tx->packets[tail % handle->capacity].len = len;
    store_release_32(&handle->tx->tail, tail + 1);
    return 0;
}

static inline int virtio_vsock_queue_peek(virtio_vsock_queue_handle_t *handle, void **data, uint32_t *len)
{
    if (virtio_vsock_queue_empty_rx(handle)) {
        return -1;
    }

    uint32_t head = handle->rx->head;
    *len = handle->rx->packets[head % handle->capacity].len;
    *data = handle->rx_data + (head % handle->capacity) * handle->buffer_size;
    return 0;
}

static inline int virtio_vsock_queue_dequeue(virtio_vsock_queue_handle_t *handle)
{
    if (virtio_vsock_queue_empty_rx(handle)) {
        return -1;
    }

    store_release_32(&handle->rx->head, handle->rx->head + 1);
    return 0;
}
