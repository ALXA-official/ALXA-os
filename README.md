# Magnesium OS v.A2 (version alpha 2)

Made by **alxa**.

> Alpha software: expect rough edges and missing features. Do not use it for
> anything important, and keep backups of `disk.img`.

An extremely minimal graphical operating system: the Linux kernel plus one
statically linked custom program (init, graphical setup & login, shell, editor,
pixel desktop, windowed terminal, registry and ~90 commands). No third-party
desktop environment, no shared libraries, no distribution bloat.

New in v.A2: **VaporGlass** frosted-glass desktop with smooth anti-aliased
fonts, 8 themes + 8 accents + 6 wallpapers, granular manual customization
(`mg.conf`), TCP/IP networking (DHCP/DNS/ping/HTTP), broad hardware driver
support, and built-in apps in `/programs` (Files, Notes, Calculator, Canvas,
System Monitor, Network Center, Mg Mines).

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

## Run online in your browser (no local install)

1. On GitHub, click **Code -> Codespaces -> Create codespace** on your branch.
2. Once the terminal opens, run:

       ./run-web.sh

   (`run-web.sh` automatically grabs the latest pre-built `magnesium-os`
   artifact from GitHub Actions if available, or builds locally, and serves the
   live pixel display + mouse + keyboard over noVNC on port `6080`.)
3. Click the **Open in Browser** prompt for port `6080` (or open the forwarded
   port `6080` URL from the **Ports** tab). You can also test no-GPU-driver
   fallback mode in the browser with `GPU=basic ./run-web.sh`.

> Note: browser-only emulators like `copy.sh/v86` are 32-bit x86 (`i686`) only
> and cannot boot Magnesium OS's 64-bit (`x86_64`) kernel. Use GitHub Codespaces
> + `./run-web.sh` (or any cloud Linux VM) to run the real 64-bit OS with KVM
> and `virtio-gpu` in your browser.

## Build and run locally

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

## Bootable ISO

After `./build.sh`, package the kernel and initramfs as a bootable ISO
(BIOS and UEFI, booted by the [Limine](https://limine-bootloader.org) bootloader):

    pip install pycdlib
    python3 make-iso.py          # writes magnesium-os-v.A2.iso

The ISO needs no xorriso or grub. `make-iso.py` downloads Limine once, pinned to
a specific commit, into `build/limine/`. Burn the ISO to a CD/DVD or attach it to
a VM as a CD-ROM. Writing it to a USB stick with `dd` is not supported yet.

    qemu-system-x86_64 -m 256M -vga std -cdrom magnesium-os-v.A2.iso \
      -nic user,model=virtio-net-pci -serial stdio

The ISO runs without a data disk, so accounts and settings are lost at reboot.
To keep them, attach a `disk.img` as a second drive, as `run-qemu.sh` does.

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
  resolution switching, theme, accent, wallpaper), Manual (granular colors,
  glass, metrics), Network, Drivers, Taskbar, Date & time, Apps, Accounts,
  Storage, Security, Terminal, and Advanced.
- **Registry Editor:** browse, add, edit and delete every stored value.
- **Desktop icons:** shortcuts to every `/programs` app, plus a right-click
  context menu (open `/programs`, next theme, toggle glass, edit `mg.conf`).

Keys: `Ctrl+G` (`F1`) Start, `Ctrl+N` (`F6`) next window, `F4` close window,
`F7` show desktop, `F10` window menu, `Ctrl+Q` exit to full-screen console.

## VaporGlass, themes & manual customization

The desktop uses **VaporGlass**: translucent frosted panels with a real
backdrop-blur of the wallpaper behind them, smooth anti-aliased fonts
(Smooth / Subpixel / Crisp modes), and the original **12 Mg** periodic-table
design language (element tiles, atomic-number badges) — no copied Aero assets.

- **8 themes:** Magnesium, Frost, Aurora, Obsidian, Ember, Cobalt, Sakura,
  Classic — plus 8 accents and 6 wallpapers (Aurora, Gradient, Nebula, Prism,
  Grid, Solid). Switch in **Settings > Display**, with `theme`, or in `mg.conf`.
- **Manual customization:** **Settings > Manual** edits every color as
  `#RRGGBB`, glass opacity/blur, corner roundness, font smoothing, taskbar
  height, window metrics, and custom wallpaper gradients. Everything is stored
  in the registry and in `mg.conf` (`/data/etc/mg.conf` for the system,
  `~/.mg.conf` per user), so it survives reboots and can be edited in Notes.

## Networking

The kernel ships a full TCP/IP stack and NIC drivers (VirtIO-Net, Intel
e1000/e1000e/igb, Realtek 8139/r8169, AMD PCnet, Hyper-V, USB Ethernet).
`run-qemu.sh` / `run-web.sh` attach a QEMU user-mode (SLIRP) NIC, so DHCP and
DNS work out of the box.

    dhcp [IFACE]            acquire an IPv4 lease (auto-run at boot)
    ip                      show interfaces, addresses, drivers, RX/TX
    ip set IFACE IP [MASK] [GW] [DNS]   manual static IPv4
    dns HOSTNAME            resolve a hostname (native UDP DNS, no libc NSS)
    ping HOST [COUNT]       ICMP ping with TCP-80 fallback
    fetch http://HOST/PATH  HTTP/1.1 client; prints page text
    netstat                 interfaces + routes + DNS

The **Network Center** app (desktop, `/programs/netcenter`) does the same in a
window: DHCP Auto, Ping Gateway/DNS, and a built-in HTTP web reader.
**Settings > Network** shows live interface status and lets you switch between
DHCP and manual static configuration.

## Drivers & hardware inspection

Storage, bus, input and filesystem drivers are built in: NVMe, SATA/AHCI,
USB mass storage, VFAT (FAT12/16/32) + ext2 for `/data`, RTC, and the full
evdev/USB-HID input stack.

    drivers                 every detected device, its bound driver, and status
    lspci                   PCI bus scan with vendor/device names

**Settings > Drivers** shows the same scan graphically, including the active
GPU driver (or the Basic Display `simpledrm` fallback).

## Built-in apps (/programs)

Seven standalone apps are compiled from `programs/` into `/programs` at build
time. They run by name from any shell (`PATH` includes `/programs`), appear as
desktop icons and Start-menu tiles, and open native VaporGlass windows when
launched from the desktop (or run in console mode with `--cli`):

| App | What it does |
|---|---|
| `files` | File manager: Places sidebar, navigate, create files/folders |
| `notes` | Text editor with line numbers (great for editing `mg.conf`) |
| `calc` | Calculator (`calc 2+2` in console, interactive `calc --cli`) |
| `canvas` | Drawing pad: pen/line/rect/circle/fill, colors, save as PPM |
| `sysmon` | System & driver monitor: CPU/RAM, processes, hardware scan |
| `netcenter` | Network Center + HTTP web reader |
| `mines` | Mg Mines: the classic minesweeper game, Magnesium style |

## The shell

| Group | Commands |
|---|---|
| View / search | `ls dir cat more head tail find grep wc sort uniq cut tr rev tac nl tee hexdump` |
| Files / folders | `cp mv rm del mkdir rmdir touch ln chmod chown stat du df basename dirname cd pwd` |
| Editor | `edit FILE` (Ctrl+O save, Ctrl+X exit) |
| System | `mem free uptime date uname hostname dmesg ps kill sleep seq sync mount umount env export unset reboot halt poweroff` |
| Display / GPU | `display gpu desktop theme` |
| Network | `ip ifconfig dhcp ping dns fetch netstat` |
| Hardware | `drivers lspci` |
| Accounts | `id whoami users passwd adduser deluser exit` |
| Registry | `reg` |
| Shell | `echo clear true false history source sh ver commands which install help` |

### Adding commands (apps)

Any executable in `/programs`, `/bin`, `/usr/bin`, `/usr/local/bin`, `/sbin`,
`/data/programs` or `/data/bin` runs by its name:

- **At build time:** put it in `overlay/bin/`; everything in `overlay/` is copied into the OS.
  Built-in desktop apps live in `programs/` and become `/programs/*`.
- **While running (root only):** `install ./myapp` copies it to `/data/programs` (saved)
  or `/programs` (RAM only, when no disk is attached).

Programs must be **statically linked** (for example `gcc -static`), because
Magnesium OS contains no shared libraries.

## The registry

Settings and system variables live in the registry: **System** (`/data/etc/registry`,
everyone can read, only root can change) and **User** (`~/.mg-registry`, private).

    reg list [KEY]      reg get KEY      reg set KEY VALUE      reg del KEY

## Tests

`tests/vm-test.sh` boots the system twice in QEMU and checks first-start setup,
login, permissions, and that accounts and files survive a reboot.
`tests/net-test.sh` boots with a virtio-net NIC and checks the driver scan,
DHCP lease, DNS resolution, and the `/programs` apps. GitHub Actions also
boot-tests both Basic Display (`nomodeset` VESA framebuffer) and native
VirtIO-GPU KMS modes on every push.

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
