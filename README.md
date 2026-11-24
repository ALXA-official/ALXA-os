# Magnesium OS v.A1 (version alpha 1)

Made by **alxa**.

> Alpha software: expect rough edges and missing features. Do not use it for
> anything important, and keep backups of `disk.img`.

An extremely minimal operating system: the Linux kernel plus one custom program
(init, login, shell, editor, desktop, registry and about 70 commands). No desktop environment, no
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
| Desktop and registry | `desktop reg` |
| Shell | `echo clear true false history source sh ver commands which install help` |

The prompt shows where you are: `/ ---->` at the top, `/home ---->` inside a
folder (the arrow is red for root). Change it in Settings > Terminal or with
`reg set Shell/Prompt Classic` and `reg set Shell/ShowUser 1`.

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

## The desktop

Type `desktop` (or turn on *Open the desktop at login* in Settings > System).
It is a text-mode desktop: it draws with colors and boxes in the terminal, so it
works over the serial console, in QEMU `-nographic`, in Codespaces and on a VGA
screen. It needs a terminal that understands mouse reports (VS Code, most Linux
and Mac terminals, Windows Terminal); without a mouse everything also works
with the keyboard. There is nothing on the desktop except what you open.

- **Taskbar** (top or bottom): the **12 Mg** Start tile, pinned apps, then
  `X` close the active window, `O` go to the desktop, `-` hide the window, and the clock.
- **Start menu:** a grid of *element tiles* (number, two-letter symbol, name).
  Type to search. Programs you install appear here too.
- **Windows:** drag the title to move, drag any border or the bottom-right
  corner to resize. Each window has a menu bar (File, Edit, View).
- **Settings:** open it and you see every category at once (System, Display,
  Taskbar, Date & time, Apps, Accounts, Storage, Security, Terminal, Advanced).
  Pick one: categories on the left, the settings on the right.
- **Registry Editor:** browse, add, edit and delete every stored value.

Keys: `Ctrl+G` Start, `Ctrl+N` next window, `Ctrl+E` close, `Ctrl+D` desktop,
`Ctrl+T` move mode, `Ctrl+R` resize mode, `Ctrl+Q` back to the command line,
`F10` window menu. Arrow keys, Enter, Tab, Esc and Del work inside windows.

## The registry

Settings and system variables live in the registry, a set of named values like
`Taskbar/Position`. There are two hives: **System** (`/data/etc/registry`,
everyone can read, only root can change) and **User** (`~/.mg-registry`,
private). They are real: `Environment/NAME` values become environment
variables at login, `Security/MinPasswordLength` and `Security/LoginDelay`
control password rules and the failed-login delay.

    reg list [KEY]      reg get KEY      reg set KEY VALUE      reg del KEY

Keys start with `System/` or `User/` (default `User/`). Example:
`reg set System/Security/MinPasswordLength 12` (as root).

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
