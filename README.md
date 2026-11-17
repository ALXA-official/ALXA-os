# ALXA OS

Made by **alxa**.

An extremely minimal operating system: the Linux kernel plus a single custom
`init` program that includes its own small shell. No desktop environment, no
third-party shell, no applications.

## The ALXA shell

After boot you get an `alxa>` prompt. Type `help` to list commands.

| Command | What it does |
|---|---|
| `ls`, `dir` | list a folder (`dir` shows sizes) |
| `cat FILE` | print a file |
| `edit FILE` | full-screen colored text editor (Ctrl+O save, Ctrl+X exit) |
| `mkdir`, `rmdir` | make / remove a folder |
| `rm`, `del` | delete a file |
| `cp SRC DST` | copy a file |
| `cd`, `pwd` | change / show the current folder |
| `mem`, `uptime`, `ver`, `clear`, `echo` | system info and basics |
| `commands` | list every available command |
| `which NAME` | show where a command comes from |
| `install FILE [NAME]` | add a program to `/bin` so it becomes a command |
| `reboot`, `poweroff` | restart / shut down |

The shell and editor are original code written for this project, not copied from
BusyBox or any other shell.

### Adding commands (apps)

Any executable file in `/bin`, `/usr/bin`, `/usr/local/bin` or `/sbin` runs by
its name, like on Linux. Two ways to add one:

- **At build time:** put the program in `overlay/bin/` in this repo. Everything in
  `overlay/` is copied into the OS image.
- **While running:** `install ./myapp` copies it into `/bin`.

Programs must be **statically linked** (for example `gcc -static`), because ALXA
OS contains no shared libraries. Note: the filesystem lives in RAM, so files
created or installed while running disappear on reboot.

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
