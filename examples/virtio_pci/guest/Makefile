# Copyright 2026
# SPDX-License-Identifier: BSD-2-Clause

ARCH := arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
JOBS ?= $(shell nproc)

GO_VERSION ?= 1.26.0
GO_ARCHIVE := go$(GO_VERSION).linux-amd64.tar.gz
GO_SHA256 := aac1b08a0fb0c4e0a7c1555beb7b59180b05dfc5a3d62e40e9de90cd42f88235
GO_URL := https://go.dev/dl/$(GO_ARCHIVE)
KUBERNETES_VERSION ?= v1.37.0
KUBERNETES_SEMVER := $(patsubst v%,%,$(KUBERNETES_VERSION))
KUBERNETES_MAJOR := $(word 1,$(subst ., ,$(KUBERNETES_SEMVER)))
KUBERNETES_MINOR := $(word 2,$(subst ., ,$(KUBERNETES_SEMVER)))
KUBERNETES_ARCHIVE := kubernetes-$(KUBERNETES_VERSION)-src.tar.gz
KUBERNETES_SHA256 := 054e46db628c510a9d0d78cf755720f3b325a1176dd7d18fe97776f9053f006a
KUBERNETES_URL := https://dl.k8s.io/$(KUBERNETES_VERSION)/kubernetes-src.tar.gz

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
VSOCK_SHIM := $(OUT)/vsock-shim
UDS_CLIENT := $(OUT)/uds-client
DOWNLOADS := $(OUT)/downloads
GO_ROOT := $(OUT)/toolchain/go
GO := $(GO_ROOT)/bin/go
GO_STAMP := $(GO_ROOT)/.installed
KUBERNETES_SRC := $(OUT)/src/kubernetes-$(KUBERNETES_VERSION)
KUBERNETES_SRC_STAMP := $(KUBERNETES_SRC)/.extracted
KUBELET := $(OUT)/kubelet
CRI_STUB := $(OUT)/cri-stub
KUBELET_KUBECONFIG ?= $(OUT)/credentials/kubelet.conf
KUBELET_CREDENTIAL_DEP := $(wildcard $(KUBELET_KUBECONFIG))

.PHONY: all kernel rootfs kubelet cri-stub check-sources clean

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

kubelet: $(KUBELET)

cri-stub: $(CRI_STUB)

$(DOWNLOADS)/$(GO_ARCHIVE):
	mkdir -p $(DOWNLOADS)
	curl --fail --location --retry 3 $(GO_URL) -o $@.tmp
	echo "$(GO_SHA256)  $@.tmp" | sha256sum --check -
	mv $@.tmp $@

$(GO_STAMP): $(DOWNLOADS)/$(GO_ARCHIVE)
	rm -rf $(GO_ROOT)
	mkdir -p $(dir $(GO_ROOT))
	tar -C $(dir $(GO_ROOT)) -xzf $<
	test "$$($(GO) version)" = "go version go$(GO_VERSION) linux/amd64"
	touch $@

$(DOWNLOADS)/$(KUBERNETES_ARCHIVE):
	mkdir -p $(DOWNLOADS)
	curl --fail --location --retry 3 $(KUBERNETES_URL) -o $@.tmp
	echo "$(KUBERNETES_SHA256)  $@.tmp" | sha256sum --check -
	mv $@.tmp $@

$(KUBERNETES_SRC_STAMP): $(DOWNLOADS)/$(KUBERNETES_ARCHIVE)
	rm -rf $(KUBERNETES_SRC)
	mkdir -p $(KUBERNETES_SRC)
	tar -C $(KUBERNETES_SRC) -xzf $<
	test -f $(KUBERNETES_SRC)/go.mod
	touch $@

$(KUBELET): $(GO_STAMP) $(KUBERNETES_SRC_STAMP) Makefile
	mkdir -p $(OUT)/go-cache $(OUT)/go-mod-cache
	cd $(KUBERNETES_SRC) && \
		PATH=$(GO_ROOT)/bin:$$PATH \
		GOCACHE=$(OUT)/go-cache GOMODCACHE=$(OUT)/go-mod-cache \
		GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local \
		$(GO) build -mod=vendor -trimpath \
		-ldflags='-s -w -X k8s.io/component-base/version.gitVersion=$(KUBERNETES_VERSION) -X k8s.io/component-base/version.gitMajor=$(KUBERNETES_MAJOR) -X k8s.io/component-base/version.gitMinor=$(KUBERNETES_MINOR)' \
		-o $@ ./cmd/kubelet
	file $@

$(CRI_STUB): $(GO_STAMP) $(KUBERNETES_SRC_STAMP) cri_stub.go Makefile
	mkdir -p $(KUBERNETES_SRC)/_output/libvmm-cri-stub $(OUT)/go-cache $(OUT)/go-mod-cache
	cp cri_stub.go $(KUBERNETES_SRC)/_output/libvmm-cri-stub/main.go
	cd $(KUBERNETES_SRC) && \
		PATH=$(GO_ROOT)/bin:$$PATH \
		GOCACHE=$(OUT)/go-cache GOMODCACHE=$(OUT)/go-mod-cache \
		GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local \
		$(GO) build -mod=vendor -trimpath -ldflags='-s -w' \
		-o $@ ./_output/libvmm-cri-stub
	file $@

$(VSOCK_SHIM): vsock_shim.c
	mkdir -p $(OUT)
	$(CROSS_COMPILE)gcc -static -O2 -Wall -Wextra -o $@ $<

$(UDS_CLIENT): uds_client.c
	mkdir -p $(OUT)
	$(CROSS_COMPILE)gcc -static -O2 -Wall -Wextra -o $@ $<

$(INITRAMFS): $(BUSYBOX_OUT)/busybox $(VSOCK_SHIM) $(UDS_CLIENT) $(KUBELET) $(CRI_STUB) $(KUBELET_CREDENTIAL_DEP) init Makefile busybox.fragment kubelet-config.yaml passwd group
	mkdir -p $(ROOTFS)/bin $(ROOTFS)/dev $(ROOTFS)/etc/init.d $(ROOTFS)/etc/kubernetes/manifests $(ROOTFS)/mnt $(ROOTFS)/proc $(ROOTFS)/run $(ROOTFS)/sys $(ROOTFS)/tmp $(ROOTFS)/usr/share/udhcpc $(ROOTFS)/var/lib/kubelet $(ROOTFS)/var/log/pods
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) CONFIG_PREFIX=$(ROOTFS) install
	install -m 0755 init $(ROOTFS)/init
	$(RM) $(ROOTFS)/bin/vsock-hello
	install -m 0755 $(VSOCK_SHIM) $(ROOTFS)/bin/vsock-shim
	install -m 0755 $(UDS_CLIENT) $(ROOTFS)/bin/uds-client
	install -m 0755 $(KUBELET) $(ROOTFS)/bin/kubelet
	install -m 0755 $(CRI_STUB) $(ROOTFS)/bin/cri-stub
	install -m 0644 kubelet-config.yaml $(ROOTFS)/etc/kubelet-config.yaml
	install -m 0644 passwd $(ROOTFS)/etc/passwd
	install -m 0644 group $(ROOTFS)/etc/group
	@if test -f "$(KUBELET_KUBECONFIG)"; then \
		install -m 0600 "$(KUBELET_KUBECONFIG)" $(ROOTFS)/etc/kubernetes/kubelet.conf; \
	else \
		rm -f $(ROOTFS)/etc/kubernetes/kubelet.conf; \
		echo "note: building without kubelet credentials"; \
	fi
	install -m 0755 $(BUSYBOX_SRC)/examples/udhcp/simple.script $(ROOTFS)/usr/share/udhcpc/default.script
	cd $(ROOTFS) && find . -print0 | cpio --null -o --format=newc | gzip -9 > $(INITRAMFS)

clean:
	$(MAKE) -C $(LINUX_SRC) O=$(LINUX_OUT) ARCH=$(ARCH) clean
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_OUT) clean
	rm -f $(INITRAMFS)
