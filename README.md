# Magnesium OS v.A1 (version alpha 1)

Made by **alxa**.

> Alpha software: expect rough edges and missing features. Do not use it for
> anything important, and keep backups of `disk.img`.

An extremely minimal graphical operating system: the Linux kernel plus one
statically linked custom program (init, graphical setup & login, shell, editor,
pixel desktop, windowed terminal, registry and ~70 commands). No third-party
desktop environment, no shared libraries, no distribution bloat.

## Graphics architecture (Basic Display + Native GPU Drivers)

Magnesium OS uses the same two-tier graphics architecture as Linux and Windows:

1. **When no GPU driver is present (*Basic Display Adapter* mode):**
   - Runs on the firmware's **UEFI GOP** or **BIOS VESA VBE** linear pixel
     framebuffer via Linux `sysfb` + `simpledrm` (or `/dev/fb0` `vesafb`/`efifb`).
   - Renders into a cacheable RAM shadow backbuffer with dirty-rectangle blits,
     16/24/32-bit pixel conversion, and a composited software arrow cursor.
2. **When a GPU driver is present (*Hardware KMS* mode):**
   - Automatically hands over from the boot firmware framebuffer to the native
     Linux **DRM/KMS** GPU driver (`virtio_gpu`, `bochs-drm`, `vmwgfx`, `qxl`,
     `vboxvideo`, `hyperv_drm`, `cirrus`; plus bare-metal `i915`, `amdgpu`,
     `radeon`, `nouveau` when built with `WITH_HW_GPUS=1`).
   - Enables native EDID resolution detection, live modesetting in
     **Settings > Display**, double-buffered VSync page-flipping
     (`DRM_IOCTL_MODE_PAGE_FLIP`), and a GPU hardware cursor plane
     (`DRM_IOCTL_MODE_CURSOR`).

## Build and run

The easiest way is GitHub Actions: every push builds everything and uploads
`bzImage`, `initramfs.gz` and `disk.img` as the `magnesium-os` artifact.

On a Linux machine (or a Codespace):

    sudo apt install build-essential flex bison bc libelf-dev libssl-dev libcrypt-dev cpio curl xz-utils e2fsprogs qemu-system-x86
    ./build.sh
    ./run-qemu.sh

To also build bare-metal Intel/AMD/NVIDIA KMS drivers into `bzImage`:

    WITH_HW_GPUS=1 ./build.sh

`run-qemu.sh` lets you test every display path directly:

    ./run-qemu.sh               # Default: native VirtIO-GPU KMS driver (1280x800, HW cursor, page-flip)
    GPU=std ./run-qemu.sh       # QEMU Standard VGA with native bochs-drm KMS driver
    GPU=basic ./run-qemu.sh     # No-GPU-driver mode (nomodeset + VESA 1024x768x32 firmware framebuffer)
    GPU=serial ./run-qemu.sh    # Headless serial console (-nographic; exit with Ctrl+A then X)

## First start, accounts and security

1. On the very first boot there are no accounts. Magnesium OS opens a graphical
   setup wizard (or text prompt on headless serial) asking you to create the
   administrator (`root`) password, then optionally a regular user. **There is
   no default password.** Passwords need 8+ characters.
2. After that you get the graphical Magnesium sign-in screen. Local display and
   input device permissions (`/dev/dri/*`, `/dev/fb0`, `/dev/input/*`) are
   automatically granted to the signed-in console user and revoked on logout.
3. Passwords are stored only as salted SHA-512 hashes, in a file only root can read.
4. Regular users cannot read other users' home folders (`0700`), cannot touch
   system files, and cannot reboot or mount. Use a regular user for daily work.
5. `passwd` changes your own password (it is a small setuid-root helper).
   Root can use `adduser NAME`, `deluser [-r] NAME`, `users`, `chown`.

## Saving to disk

`disk.img` is a blank 32 MB ext2 disk. When it is attached (`run-qemu.sh` does
this automatically), Magnesium OS mounts it on `/data` and keeps your **accounts,
home folders and installed programs** there, so they survive reboots.
Without a disk everything lives in RAM and is lost on reboot.

Keep a backup of `disk.img`: ext2 has no journal, so shutting down by closing
QEMU in the middle of a write can corrupt it. Use Restart / Shut down or
`reboot` / `poweroff` when you can.

## The pixel desktop

Logging in on a graphical screen opens the Magnesium pixel desktop automatically
(configurable in **Settings > System**, or launch anytime from the console with
`desktop`).

- **Taskbar** (top or bottom): the **12 Mg** Start button, pinned apps
  (`Terminal`, `Settings`, `Registry`), open windows, `×` close, `O` show
  desktop, `—` minimize, and the clock.
- **Start menu:** a grid of Periodic Table *element tiles* (atomic number,
  two-letter symbol, name). Type to search. Installed programs appear here too.
- **Windows:** pixel-rendered rounded windows with drop shadows, titlebars,
  minimize/close buttons, menu bars, and draggable edges/corners to resize.
- **Terminal:** a real windowed graphical terminal backed by a Linux PTY
  (`/dev/ptmx`) running the Magnesium shell inside the desktop.
- **Settings:** System, Display (adapter status, pipeline info, live KMS
  resolution switching, theme, accent, wallpaper), Taskbar, Date & time, Apps,
  Accounts, Storage, Security, Terminal, and Advanced.
- **Registry Editor:** browse, add, edit and delete every stored value.

Keys: `Ctrl+G` (`F1`) Start, `Ctrl+N` (`F6`) next window, `F4` close window,
`F7` show desktop, `F10` window menu, `Ctrl+Q` exit to full-screen console.

## The shell

| Group | Commands |
|---|---|
| View / search | `ls dir cat more head tail find grep wc sort uniq cut tr rev tac nl tee hexdump` |
| Files / folders | `cp mv rm del mkdir rmdir touch ln chmod chown stat du df basename dirname cd pwd` |
| Editor | `edit FILE` (Ctrl+O save, Ctrl+X exit) |
| System | `mem free uptime date uname hostname dmesg ps kill sleep seq sync mount umount env export unset reboot halt poweroff` |
| Display / GPU | `display gpu desktop` |
| Accounts | `id whoami users passwd adduser deluser exit` |
| Registry | `reg` |
| Shell | `echo clear true false history source sh ver commands which install help` |

### Adding commands (apps)

Any executable in `/bin`, `/usr/bin`, `/usr/local/bin`, `/sbin` or `/data/bin`
runs by its name:

- **At build time:** put it in `overlay/bin/`; everything in `overlay/` is copied into the OS.
- **While running (root only):** `install ./myapp` copies it to `/data/bin` (saved)
  or `/bin` (RAM only, when no disk is attached).

Programs must be **statically linked** (for example `gcc -static`), because
Magnesium OS contains no shared libraries.

## The registry

Settings and system variables live in the registry: **System** (`/data/etc/registry`,
everyone can read, only root can change) and **User** (`~/.mg-registry`, private).

    reg list [KEY]      reg get KEY      reg set KEY VALUE      reg del KEY

## Tests

`tests/vm-test.sh` boots the system twice in QEMU and checks first-start setup,
login, permissions, and that accounts and files survive a reboot. GitHub
Actions also boot-tests both Basic Display (`nomodeset` VESA framebuffer) and
native VirtIO-GPU KMS modes on every push.

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
