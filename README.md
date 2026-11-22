# Magnesium OS v.A1 (version alpha 1)

Made by **alxa**.

> Alpha software: expect rough edges and missing features. Do not use it for
> anything important, and keep backups of `disk.img`.

An extremely minimal operating system: the Linux kernel plus one custom program
(init, login, shell, editor and about 70 commands). No desktop environment, no
third-party shell, no distribution, no applications.

## Build and run

The easiest way is GitHub Actions: every push builds everything and uploads
`bzImage`, `initramfs.gz` and `disk.img` as the `magnesium-os` artifact.

On a Linux machine (or a Codespace):

    sudo apt install build-essential flex bison bc libelf-dev libssl-dev libcrypt-dev cpio curl xz-utils e2fsprogs qemu-system-x86
    ./build.sh
    ./run-qemu.sh

Exit QEMU with `Ctrl+A` then `X`. To run a downloaded build:

    qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz -append "console=ttyS0" -nographic -m 128M -drive file=disk.img,if=virtio,format=raw

## First start, accounts and security

1. On the very first boot there are no accounts. Magnesium OS asks you to create the
   administrator (`root`) password, then optionally a regular user. **There is
   no default password.** Passwords need 8+ characters.
2. After that you get an `magnesium login:` prompt. Failed logins are slowed down.
3. Passwords are stored only as salted SHA-512 hashes, in a file only root can read.
4. Regular users cannot read other users' home folders (`0700`), cannot touch
   system files, and cannot reboot or mount. Use a regular user for daily work.
5. `passwd` changes your own password (it is a small setuid-root helper).
   Root can use `adduser NAME`, `deluser [-r] NAME`, `users`, `chown`.
6. Type `exit` or press Ctrl+D to log out.

## Saving to disk

`disk.img` is a blank 32 MB ext2 disk. When it is attached (`run-qemu.sh` does
this automatically), Magnesium OS mounts it on `/data` and keeps your **accounts,
home folders and installed programs** there, so they survive reboots.
Without a disk everything lives in RAM and is lost on reboot.

Keep a backup of `disk.img`: ext2 has no journal, so shutting down by closing
QEMU in the middle of a write can corrupt it. Use `reboot` when you can.

## The shell

| Group | Commands |
|---|---|
| View / search | `ls dir cat more head tail find grep wc sort uniq cut tr rev tac nl tee hexdump` |
| Files / folders | `cp mv rm del mkdir rmdir touch ln chmod chown stat du df basename dirname cd pwd` |
| Editor | `edit FILE` (Ctrl+O save, Ctrl+X exit) |
| System | `mem free uptime date uname hostname dmesg ps kill sleep seq sync mount umount env export unset reboot halt poweroff` |
| Accounts | `id whoami users passwd adduser deluser exit` |
| Shell | `echo clear true false history source sh ver commands which install help` |

Syntax: pipes `a | b`, redirects `> >> <`, `a ; b`, `a && b`, `a || b`,
background `a &`, comments `#`, quotes, `$VAR`, `$?`, `~`, wildcards `* ? []`.

Keys: Up/Down history, Tab completion, Ctrl+C stops a running program or
cancels the line, Ctrl+D logs out.

Scripts: a file starting with `#!` that is executable runs with the Magnesium shell
(`source FILE` runs it inside the current shell).

Not included: `awk`, `sed`, `tar`, `vi`, networking, `su`/`sudo`, `if`/`for`
in scripts, positional parameters (`$1`).

### Adding commands (apps)

Any executable in `/bin`, `/usr/bin`, `/usr/local/bin`, `/sbin` or `/data/bin`
runs by its name. Ways to add one:

- **At build time:** put it in `overlay/bin/`; everything in `overlay/` is copied into the OS.
- **While running (root only):** `install ./myapp` copies it to `/data/bin` (saved)
  or `/bin` (RAM only, when no disk is attached).

Programs must be **statically linked** (for example `gcc -static`), because
Magnesium OS contains no shared libraries.

## Tests

`tests/vm-test.sh` boots the system twice in QEMU and checks first-start setup,
login, permissions, and that accounts and files survive a reboot. GitHub
Actions runs it on every push.

## Licensing

- Code in this repository is MIT licensed (see `LICENSE`). It is original work,
  not copied from BusyBox or any other GPL project.
- The Linux kernel is **not** included. It is downloaded from kernel.org at
  build time and is licensed under GPL-2.0. If you distribute a kernel binary
  built from it, you must also provide the corresponding kernel source and
  `kernel.config`.
- Password hashing uses the C library's `crypt()` (libxcrypt, LGPL). The
  binaries are statically linked; if you distribute them, keep the LGPL
  obligations of the libraries in mind.
- "Linux" is a trademark of Linus Torvalds. Magnesium OS is an independent project
  and is not affiliated with or endorsed by the Linux kernel project or the
  Linux Foundation.
