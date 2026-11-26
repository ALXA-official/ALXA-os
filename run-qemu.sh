#!/usr/bin/env bash
# Boots Magnesium OS in QEMU in pixel graphics mode.
#
# Choose a display mode with GPU=...:
#   GPU=virtio ./run-qemu.sh   (default) Native VirtIO-GPU KMS driver: hardware cursor,
#                              double-buffered VSync page-flip, live resolution changes
#   GPU=std    ./run-qemu.sh   QEMU standard VGA with native bochs-drm KMS driver
#   GPU=basic  ./run-qemu.sh   No-GPU-driver mode (nomodeset + VESA 1024x768x32 linear FB):
#                              runs on simpledrm/fbdev just like Windows Basic Display Adapter
#   GPU=serial ./run-qemu.sh   Headless text console over serial (-nographic, Ctrl+A then X)

GPU="${GPU:-virtio}"
MEM="${MEM:-256M}"

# User-mode (SLIRP) networking: a virtio-net NIC with DHCP/DNS served by QEMU,
# so `dhcp`, `ping`, `dns` and `fetch` work out of the box in the shell.
NIC=(-nic user,model=virtio-net-pci)

DISK=()
[ -f disk.img ] && DISK=(-drive file=disk.img,if=virtio,format=raw)

KVM=()
[ -w /dev/kvm ] && KVM=(-enable-kvm -cpu host)

case "$GPU" in
  serial)
    exec qemu-system-x86_64 "${KVM[@]}" -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0" -nographic -m "$MEM" "${NIC[@]}" "${DISK[@]}"
    ;;
  basic|none)
    # Disable PCI KMS GPU drivers (`nomodeset`) and request BIOS VESA 1024x768x32 (0x318 = 792)
    # so Magnesium OS runs on the firmware linear framebuffer (`simpledrm` / `fbdev`).
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "vga=792 nomodeset console=ttyS0 console=tty0" \
      -vga std -usb -device usb-tablet -serial stdio "${NIC[@]}" "${DISK[@]}"
    ;;
  std|bochs)
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0 console=tty0" \
      -vga std -usb -device usb-tablet -serial stdio "${NIC[@]}" "${DISK[@]}"
    ;;
  virtio|*)
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0 console=tty0" \
      -vga none -device virtio-gpu-pci,xres=1280,yres=800 \
      -usb -device usb-tablet -serial stdio "${NIC[@]}" "${DISK[@]}"
    ;;
esac
