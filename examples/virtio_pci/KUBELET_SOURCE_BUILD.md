# Building an ARM64 kubelet into the virtio_pci guest

The source-built guest pins and verifies the following upstream inputs:

- Go 1.26.0 for Linux/amd64 (the build toolchain)
- Kubernetes v1.37.0 source
- an `linux/arm64`, statically linked kubelet

Downloads, extracted sources, compiler caches, and binaries are kept below
`build/source_guest`. They are build artifacts and are not committed.

## Prerequisites

The existing source guest prerequisites still apply: an AArch64 cross compiler,
the Xhute Linux and BusyBox inputs, the Microkit SDK, and the repository's
Python environment containing sdfgen 0.35.x.

```sh
cd ~/wsp/libvmm/examples/virtio_pci
source ~/wsp/microkit/pyenv/bin/activate
export MICROKIT_SDK=~/wsp/microkit/release/microkit-sdk-2.3.0-dev
export MICROKIT_BOARD=qemu_virt_aarch64
```

## Build targets

To fetch the checksummed upstream archives and build only kubelet:

```sh
make source-kubelet
```

To build Linux, BusyBox, kubelet, the initramfs, and the complete Microkit
image:

```sh
make source-build
```

To build and start QEMU:

```sh
make source-qemu
```

The kubelet is installed as `/bin/kubelet`; its initial configuration is
`/etc/kubelet-config.yaml`. It is deliberately not started by `/init` yet.
Starting it usefully also requires a node kubeconfig and a CRI v1 endpoint;
those belong to the control-plane/CRI integration stage rather than the
source-build mechanism.

## Overrides

Versions and source locations remain Make variables. For example:

```sh
make source-kubelet KUBERNETES_VERSION=v1.37.0 GO_VERSION=1.26.0
make source-build LINUX_SRC=/path/to/linux BUSYBOX_TARBALL=/path/to/busybox.tar.bz2
```

When changing a pinned upstream version, update its corresponding SHA-256 in
`guest/Makefile`; the build refuses an archive whose digest does not match.

## Guest memory layout

The guest now has 512 MiB of RAM. Its compressed initramfs begins at guest
physical address `0x50000000`; the build computes the exact `linux,initrd-end`
value before compiling the DTB and checks that the image cannot overlap it.
This replaces the example's former fixed 16 MiB initramfs window.
