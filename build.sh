#!/usr/bin/env bash
# Magnesium OS - builds a Linux kernel + initramfs (+ a blank data disk) with:
#   - Basic Display fallback when no GPU driver is present (UEFI GOP & BIOS VESA
#     linear framebuffer via sysfb + simpledrm + fbdev)
#   - Native DRM/KMS GPU drivers when hardware matches (VirtIO-GPU, Bochs/stdvga,
#     QXL, VMware SVGA, VirtualBox, Hyper-V; set WITH_HW_GPUS=1 to also build
#     bare-metal Intel i915, AMD amdgpu/radeon, and NVIDIA nouveau KMS drivers)
#   - Full evdev input stack (PS/2, VirtIO input, USB XHCI/EHCI/UHCI HID mice,
#     tablets & keyboards) and UNIX98 PTYs for the windowed graphical terminal.
set -euo pipefail

KERNEL_VERSION="${KERNEL_VERSION:-6.12.1}"
WITH_HW_GPUS="${WITH_HW_GPUS:-0}"
MAJOR="${KERNEL_VERSION%%.*}"
JOBS="$(nproc)"

mkdir -p build && cd build

if [ ! -d "linux-$KERNEL_VERSION" ]; then
  curl -LO "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KERNEL_VERSION}.tar.xz"
  tar xf "linux-${KERNEL_VERSION}.tar.xz"
fi

# 1) Kernel: start from tinyconfig, enable pixel graphics + GPU KMS + fallback FB + input
cd "linux-$KERNEL_VERSION"
make tinyconfig

HW_GPU_ARGS=()
if [ "$WITH_HW_GPUS" = "1" ]; then
  HW_GPU_ARGS=(
    --enable AGP
    --enable AGP_INTEL
    --enable DRM_I915
    --enable DRM_RADEON
    --enable DRM_AMDGPU
    --enable DRM_NOUVEAU
  )
fi

./scripts/config \
  --enable 64BIT \
  --enable SMP \
  --enable PRINTK \
  --enable BUG \
  --enable ELF_CORE \
  --enable FUTEX \
  --enable EPOLL \
  --enable SIGNALFD \
  --enable TIMERFD \
  --enable EVENTFD \
  --enable SHMEM \
  --enable TMPFS \
  --enable SYSFS \
  --enable PROC_FS \
  --enable DEVTMPFS \
  --enable DEVTMPFS_MOUNT \
  --enable BLK_DEV_INITRD \
  --enable RD_GZIP \
  --enable BINFMT_ELF \
  --enable BINFMT_SCRIPT \
  --enable MULTIUSER \
  --enable BLOCK \
  --enable BLK_DEV \
  --enable EXT2_FS \
  --enable TTY \
  --enable UNIX98_PTYS \
  --enable VT \
  --enable CONSOLE_TRANSLATIONS \
  --enable VT_CONSOLE \
  --enable VT_HW_CONSOLE_BINDING \
  --enable SERIAL_8250 \
  --enable SERIAL_8250_CONSOLE \
  --enable PCI \
  --enable PCI_MSI \
  --enable ACPI \
  --enable PNP \
  --enable PNPACPI \
  --enable EFI \
  --enable EFI_STUB \
  --enable BOOT_VESA_SUPPORT \
  --enable FIRMWARE_EDID \
  --enable SYSFB \
  --enable SYSFB_SIMPLEFB \
  --enable FB \
  --enable FB_DEVICE \
  --enable FB_VESA \
  --enable FB_EFI \
  --enable FRAMEBUFFER_CONSOLE \
  --enable FONT_SUPPORT \
  --enable FONT_8x16 \
  --enable SYNC_FILE \
  --enable I2C \
  --enable BACKLIGHT_CLASS_DEVICE \
  --enable DRM \
  --enable DRM_FBDEV_EMULATION \
  --enable DRM_SIMPLEDRM \
  --enable VIRTIO_MENU \
  --enable VIRTIO_PCI \
  --enable VIRTIO_BLK \
  --enable VIRTIO_INPUT \
  --enable HYPERVISOR_GUEST \
  --enable HYPERV \
  --enable DRM_VIRTIO_GPU \
  --enable DRM_BOCHS \
  --enable DRM_QXL \
  --enable DRM_CIRRUS_QEMU \
  --enable DRM_VMWGFX \
  --enable DRM_VBOXVIDEO \
  --enable DRM_HYPERV \
  --enable SERIO \
  --enable SERIO_I8042 \
  --enable SERIO_LIBPS2 \
  --enable INPUT \
  --enable INPUT_MOUSEDEV \
  --enable INPUT_EVDEV \
  --enable INPUT_KEYBOARD \
  --enable KEYBOARD_ATKBD \
  --enable INPUT_MOUSE \
  --enable MOUSE_PS2 \
  --enable HID_SUPPORT \
  --enable HID \
  --enable HID_GENERIC \
  --enable USB_SUPPORT \
  --enable USB \
  --enable USB_PCI \
  --enable USB_XHCI_HCD \
  --enable USB_XHCI_PCI \
  --enable USB_EHCI_HCD \
  --enable USB_EHCI_PCI \
  --enable USB_UHCI_HCD \
  --enable USB_HID \
  "${HW_GPU_ARGS[@]}"

make olddefconfig

# Fail early, with a clear message, if the kernel silently dropped something we need.
for opt in 64BIT SMP TTY UNIX98_PTYS VT VT_CONSOLE SERIAL_8250_CONSOLE \
           BLK_DEV_INITRD BINFMT_ELF DEVTMPFS PROC_FS SYSFS TMPFS \
           MULTIUSER BLOCK PCI PCI_MSI ACPI EFI EFI_STUB \
           SYSFB SYSFB_SIMPLEFB FB FB_VESA FB_EFI FRAMEBUFFER_CONSOLE \
           DRM DRM_FBDEV_EMULATION DRM_SIMPLEDRM \
           DRM_VIRTIO_GPU DRM_BOCHS DRM_VMWGFX DRM_VBOXVIDEO \
           VIRTIO_PCI VIRTIO_BLK VIRTIO_INPUT EXT2_FS \
           INPUT INPUT_EVDEV KEYBOARD_ATKBD MOUSE_PS2 USB USB_XHCI_PCI USB_HID; do
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
gcc -static -O2 -o ../init \
  ../boot.c ../shell.c ../users.c ../line.c ../cmds.c \
  ../editor.c ../registry.c ../gfx.c ../gui.c -lcrypt

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
