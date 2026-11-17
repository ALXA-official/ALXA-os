# ALXA OS

Made by **alxa**.

An extremely minimal operating system: the Linux kernel plus a single custom
`init` program that includes its own small shell. No desktop environment, no
third-party shell, no applications.

## The ALXA shell

After boot you get an `alxa>` prompt. Type `help` to list commands (ls, cat,
mkdir, cd, mem, uptime, reboot and more). The shell is original code written for
this project, not copied from BusyBox or any other shell.

## Build

Requires a Linux machine (or a Linux VM). Install dependencies (Debian/Ubuntu):

    sudo apt install build-essential flex bison bc libelf-dev libssl-dev cpio curl xz-utils qemu-system-x86

Then:

    chmod +x build.sh run-qemu.sh
    ./build.sh
    ./run-qemu.sh

Exit QEMU with `Ctrl+A` then `X`.

## Licensing

- Code in this repository (`init.c`, scripts) is MIT licensed (see `LICENSE`).
- The Linux kernel is **not** included. It is downloaded from kernel.org at
  build time and is licensed under GPL-2.0. If you distribute a kernel binary
  built from it, you must also provide the corresponding kernel source and
  your `.config`.
- "Linux" is a trademark of Linus Torvalds. ALXA OS is an independent project
  and is not affiliated with or endorsed by the Linux kernel project or the
  Linux Foundation.

## Automatic builds

Every push runs `.github/workflows/build.yml`, which builds the kernel and
initramfs, boots them in QEMU to check the ALXA OS message appears, and uploads
`bzImage`, `initramfs.gz` and `kernel.config` as a downloadable artifact.
The kernel version used is set in `build.sh` (`KERNEL_VERSION`); the matching
source is available from kernel.org.
