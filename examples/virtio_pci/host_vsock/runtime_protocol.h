/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CRI_PORT 1234U
#define CRI_MAGIC 0x4352564dU
#define CRI_VERSION 1U
#define CRI_MAX_OBJECTS 32U

enum cri_op {
	CRI_HELLO = 1,
	CRI_RUN_SANDBOX = 10, CRI_STOP_SANDBOX, CRI_REMOVE_SANDBOX,
	CRI_SANDBOX_STATUS, CRI_LIST_SANDBOX,
	CRI_CREATE_CONTAINER = 20, CRI_START_CONTAINER, CRI_STOP_CONTAINER,
	CRI_REMOVE_CONTAINER, CRI_CONTAINER_STATUS, CRI_LIST_CONTAINER,
	CRI_REOPEN_LOG,
	CRI_PULL_IMAGE = 30, CRI_REMOVE_IMAGE, CRI_IMAGE_STATUS, CRI_LIST_IMAGE,
};

enum cri_result { CRI_OK, CRI_NOT_FOUND, CRI_INVALID, CRI_FULL };

/* A bounded, little-endian PoC ABI. Maps are opaque length-prefixed blobs. */
struct __attribute__((packed)) cri_object {
	uint32_t cursor, present, attempt, state, exit_code;
	uint32_t network_ns, ipc_ns, pid_ns;
	uint64_t created_at, started_at, finished_at;
	char id[64], name[128], namespace_[64], uid[64];
	uint8_t labels[512], annotations[512];
	char runtime_handler[64], log_dir[192], sandbox_id[64];
	char image[256], image_ref[256], log_path[192], reason[64], ip[48];
};

uint32_t cri_dispatch(uint16_t op, const struct cri_object *request,
		      struct cri_object *response);
