/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <stdint.h>
#include <sys/socket.h>

#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

#define VMADDR_CID_ANY  UINT32_MAX
#define VMADDR_CID_HOST 2U

struct sockaddr_vm {
	sa_family_t svm_family;
	unsigned short svm_reserved1;
	unsigned int svm_port;
	unsigned int svm_cid;
	unsigned char svm_flags;
	unsigned char svm_zero[sizeof(struct sockaddr) -
			       sizeof(sa_family_t) - sizeof(unsigned short) -
			       sizeof(unsigned int) - sizeof(unsigned int) -
			       sizeof(unsigned char)];
};
