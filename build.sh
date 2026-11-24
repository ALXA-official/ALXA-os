#!/usr/bin/env bash
# Magnesium OS - builds a tiny Linux kernel + initramfs (+ a blank data disk).
# Requires: gcc, make, flex, bison, bc, libelf-dev, libssl-dev, libcrypt-dev,
#           cpio, gzip, curl, xz, e2fsprogs (for the disk image; optional)
set -euo pipefail

# Check https://kernel.org for the latest stable/LTS version and update if you like.
KERNEL_VERSION="${KERNEL_VERSION:-6.12.1}"
MAJOR="${KERNEL_VERSION%%.*}"
JOBS="$(nproc)"

mkdir -p build && cd build

if [ ! -d "linux-$KERNEL_VERSION" ]; then
  curl -LO "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KERNEL_VERSION}.tar.xz"
  tar xf "linux-${KERNEL_VERSION}.tar.xz"
fi

# 1) Kernel: start from the smallest possible config, enable only what we need
cd "linux-$KERNEL_VERSION"
make tinyconfig
./scripts/config \
  --enable 64BIT \
  --enable PRINTK \
  --enable TTY \
  --enable SERIAL_8250 \
  --enable SERIAL_8250_CONSOLE \
  --enable BLK_DEV_INITRD \
  --enable RD_GZIP \
  --enable BINFMT_ELF \
  --enable DEVTMPFS \
  --enable DEVTMPFS_MOUNT \
  --enable PROC_FS \
  --enable MULTIUSER \
  --enable BLOCK \
  --enable BLK_DEV \
  --enable PCI \
  --enable VIRTIO_MENU \
  --enable VIRTIO_PCI \
  --enable VIRTIO_BLK \
  --enable EXT2_FS
make olddefconfig

# Fail early, with a clear message, if the kernel silently dropped something we need.
for opt in 64BIT TTY SERIAL_8250_CONSOLE BLK_DEV_INITRD BINFMT_ELF DEVTMPFS PROC_FS \
           MULTIUSER BLOCK PCI VIRTIO_PCI VIRTIO_BLK EXT2_FS; do
  if ! grep -q "^CONFIG_${opt}=y" .config; then
    echo "ERROR: kernel option CONFIG_${opt} is not enabled after 'make olddefconfig'."
    echo "       A dependency is probably missing. See kernel.config in the build folder."
    cp .config ../../kernel.config
    exit 1
  fi
done

cp .config ../../kernel.config
make -j"$JOBS" bzImage
cp arch/x86/boot/bzImage ../../bzImage
cd ..

# 2) initramfs: our programs only, statically linked (no libraries in the OS)
gcc -static -O2 -o ../init ../boot.c ../shell.c ../users.c ../line.c ../cmds.c ../editor.c ../registry.c ../gui.c -lcrypt
rm -rf rootfs && mkdir rootfs
cp ../init rootfs/init
chmod 755 rootfs/init
# /sbin/passwd is the same program; it is setuid root so users can change their
# own password (it refuses to do anything else when run setuid).
mkdir -p rootfs/sbin
cp ../init rootfs/sbin/passwd
chmod 4755 rootfs/sbin/passwd
# Anything in overlay/ is copied into the OS. Put programs in overlay/bin/ and
# they become commands (they must be statically linked: Magnesium OS has no libraries).
if [ -d ../overlay ]; then
  cp -a ../overlay/. rootfs/
  rm -f rootfs/bin/.gitkeep
fi
# --owner=0:0 makes every file belong to root inside Magnesium OS, not to whoever built it.
(cd rootfs && find . | cpio -o -H newc --owner=0:0 2>/dev/null | gzip > ../../initramfs.gz)
if zcat ../../initramfs.gz | cpio -tv 2>/dev/null | awk '{print $3}' | grep -qv '^root$'; then
  echo "WARNING: some files in the image are not owned by root; check your cpio version."
fi

# 3) a blank 32 MB data disk (ext2), used for saving files and accounts
cd ..
if command -v mke2fs >/dev/null 2>&1; then
  rm -f disk.img
  truncate -s 32M disk.img
  mke2fs -q -t ext2 -F -L MAGNESIUMDATA -E root_owner=0:0 disk.img
  echo "Done: bzImage + initramfs.gz + disk.img. Run ./run-qemu.sh"
else
  echo "Done: bzImage + initramfs.gz (no disk.img: install e2fsprogs to make one). Run ./run-qemu.sh"
fi
