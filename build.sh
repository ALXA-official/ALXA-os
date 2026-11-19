#!/usr/bin/env bash
# ALXA OS - builds a tiny Linux kernel + initramfs containing only ./init.
# Requires: gcc, make, flex, bison, bc, libelf-dev, libssl-dev, cpio, gzip, curl, xz
set -euo pipefail

# Check https://kernel.org for the latest stable/LTS version and update if you like.
KERNEL_VERSION="${KERNEL_VERSION:-6.12.1}"
MAJOR="${KERNEL_VERSION%%.*}"
JOBS="$(nproc)"

mkdir -p build && cd build

if [ ! -d "linux-$KERNEL_VERSION" ]; then
  if [ ! -f "linux-${KERNEL_VERSION}.tar.xz" ]; then
    curl -fLO "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KERNEL_VERSION}.tar.xz"
  fi
  tar xf "linux-${KERNEL_VERSION}.tar.xz"
fi

# 1) Kernel: start from the smallest possible config, enable only the essentials
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
  --enable PROC_FS
make olddefconfig
cp .config ../../kernel.config
make -j"$JOBS" bzImage
cp arch/x86/boot/bzImage ../../bzImage
cd ..

# 2) initramfs: only our init, statically linked
gcc -static -O2 -o ../init ../init.c ../cmds.c ../editor.c
rm -rf rootfs && mkdir rootfs
cp ../init rootfs/init
# Anything in overlay/ is copied into the OS. Put programs in overlay/bin/ and
# they become commands (they must be statically linked: ALXA OS has no libraries).
if [ -d ../overlay ]; then
  cp -a ../overlay/. rootfs/
  rm -f rootfs/bin/.gitkeep
fi
(cd rootfs && find . | cpio -o -H newc | gzip > ../../initramfs.gz)

echo "Done: bzImage + initramfs.gz. Run ./run-qemu.sh"
