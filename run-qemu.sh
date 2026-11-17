#!/usr/bin/env bash
exec qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz \
  -append "console=ttyS0" -nographic -m 64M
