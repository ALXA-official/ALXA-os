# ALXA OS

Made by **alxa**.

An extremely minimal operating system: the Linux kernel plus a single custom
`init` program that includes its own small shell. No desktop environment, no
third-party shell, no applications.

## The ALXA shell

After boot you get an `alxa:/>` prompt. Type `help` to list commands.

**Commands (about 70):**

| Group | Commands |
|---|---|
| View / search | `ls dir cat more head tail find grep wc sort uniq cut tr rev tac nl tee hexdump` |
| Files / folders | `cp mv rm del mkdir rmdir touch ln chmod stat du df basename dirname cd pwd` |
| Editor | `edit FILE` (Ctrl+O save, Ctrl+X exit) |
| System | `mem free uptime date uname hostname id whoami dmesg ps kill sleep seq sync mount umount env export unset reboot halt poweroff` |
| Other | `echo clear true false ver commands which install help` |

**Shell syntax:** pipes `a | b`, redirects `> >> <`, sequences `a ; b`,
quotes `"..."` and `'...'`, variables `$VAR` and `$?`.

Examples: `ls -l | grep rwx`, `cat log.txt | sort | uniq -c > counts.txt`,
`find / -name "*.c" | wc -l`.

All commands and the shell are original code written for this project, not
copied from BusyBox or any other GPL software. They are reimplementations
with the same names, with their own options and messages, so they are not
byte-for-byte clones of BusyBox. Not included (they need much more in the
kernel or are very large): networking tools, `vi`, `awk`, `sed`, `tar`.

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
