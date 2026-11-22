#!/usr/bin/env bash
# Boots Magnesium OS in QEMU. If disk.img exists it is attached as the data disk,
# so your accounts and files survive a reboot. Exit QEMU with Ctrl+A then X.
DISK=()
[ -f disk.img ] && DISK=(-drive file=disk.img,if=virtio,format=raw)
exec qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz \
  -append "console=ttyS0" -nographic -m 128M "${DISK[@]}"
