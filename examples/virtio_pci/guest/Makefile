# Copyright 2026
# SPDX-License-Identifier: BSD-2-Clause

ARCH := arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
JOBS ?= $(shell nproc)

# These defaults reuse the source checkouts used by the neighbouring Xhute
# project. Override them when using standalone Linux or BusyBox checkouts.
LINUX_SRC ?= $(abspath ../../../../Xhute/dep/linux)
OUT ?= $(abspath ../build/source_guest)
BUSYBOX_TARBALL ?= $(abspath ../../../../Xhute/repos/busybox-1.37.0.tar.bz2)
BUSYBOX_SRC ?= $(OUT)/src/busybox-1.37.0
LINUX_OUT := $(OUT)/linux
BUSYBOX_OUT := $(OUT)/busybox
ROOTFS := $(OUT)/rootfs
IMAGE := $(LINUX_OUT)/arch/arm64/boot/Image
INITRAMFS := $(OUT)/rootfs.cpio.gz
VSOCK_HELLO := $(OUT)/vsock-hello

.PHONY: all kernel rootfs check-sources clean

all: kernel rootfs

check-sources: $(BUSYBOX_SRC)/Makefile
	@test -f "$(LINUX_SRC)/Makefile" || { echo "LINUX_SRC is not a Linux source tree: $(LINUX_SRC)"; exit 1; }
	@test -f "$(BUSYBOX_SRC)/Makefile" || { echo "BUSYBOX_SRC is not a BusyBox source tree: $(BUSYBOX_SRC)"; exit 1; }

$(BUSYBOX_SRC)/Makefile:
	@test -f "$(BUSYBOX_TARBALL)" || { echo "BUSYBOX_TARBALL does not exist: $(BUSYBOX_TARBALL)"; exit 1; }
	mkdir -p $(OUT)/src
	tar -C $(OUT)/src -xf $(BUSYBOX_TARBALL)

$(LINUX_OUT)/.config: kernel.fragment | check-sources
	mkdir -p $(LINUX_OUT)
	$(MAKE) -C $(LINUX_SRC) O=$(LINUX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) tinyconfig
	$(LINUX_SRC)/scripts/kconfig/merge_config.sh -m -O $(LINUX_OUT) $@ $(abspath kernel.fragment)
	$(MAKE) -C $(LINUX_SRC) O=$(LINUX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) olddefconfig

kernel: $(IMAGE)

$(IMAGE): $(LINUX_OUT)/.config
	$(MAKE) -C $(LINUX_SRC) O=$(LINUX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -j$(JOBS) Image

$(BUSYBOX_OUT)/.config: busybox.fragment | check-sources
	mkdir -p $(BUSYBOX_OUT)
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) allnoconfig
	$(LINUX_SRC)/scripts/kconfig/merge_config.sh -m -O $(BUSYBOX_OUT) $@ $(abspath busybox.fragment)
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) oldconfig </dev/null

$(BUSYBOX_OUT)/busybox: $(BUSYBOX_OUT)/.config
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -j$(JOBS)

rootfs: $(INITRAMFS)

$(VSOCK_HELLO): vsock_hello.c
	mkdir -p $(OUT)
	$(CROSS_COMPILE)gcc -static -O2 -Wall -Wextra -o $@ $<

$(INITRAMFS): $(BUSYBOX_OUT)/busybox $(VSOCK_HELLO) init Makefile busybox.fragment
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/dev $(ROOTFS)/etc/init.d $(ROOTFS)/mnt $(ROOTFS)/proc $(ROOTFS)/sys $(ROOTFS)/tmp $(ROOTFS)/usr/share/udhcpc
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) CONFIG_PREFIX=$(ROOTFS) install
	install -m 0755 init $(ROOTFS)/init
	install -m 0755 $(VSOCK_HELLO) $(ROOTFS)/bin/vsock-hello
	install -m 0755 $(BUSYBOX_SRC)/examples/udhcp/simple.script $(ROOTFS)/usr/share/udhcpc/default.script
	cd $(ROOTFS) && find . -print0 | cpio --null -o --format=newc | gzip -9 > $(INITRAMFS)

clean:
	$(MAKE) -C $(LINUX_SRC) O=$(LINUX_OUT) ARCH=$(ARCH) clean
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) clean
	rm -f $(INITRAMFS)
