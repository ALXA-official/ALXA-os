#!/usr/bin/env python3
"""Package Magnesium OS as a bootable ISO 9660 image (BIOS + UEFI).

The ISO boots the Limine bootloader (https://limine-bootloader.org), which loads
the kernel (bzImage) and initramfs.gz produced by ./build.sh.

Usage:
    python3 make-iso.py                      # uses ./bzImage and ./initramfs.gz
    python3 make-iso.py -k build/out/bzImage -i build/out/initramfs.gz -o os.iso

Requires: python3 and `pip install pycdlib` (pure Python, no xorriso needed).
Limine's prebuilt binaries are downloaded once from codeload.github.com into
build/limine/ (pinned to a specific commit of the v8.x-binary branch).

The live system runs without a disk: accounts and settings are lost at reboot.
Attach a data disk (see README) to keep them.
"""

import argparse
import io
import os
import sys
import tarfile
import urllib.request

LIMINE_COMMIT = "aad3edd370955449717a334f0289dee10e2c5f01"  # limine v8.x-binary
LIMINE_URL = f"https://codeload.github.com/limine-bootloader/limine/tar.gz/{LIMINE_COMMIT}"
LIMINE_FILES = ["limine-bios-cd.bin", "limine-bios.sys", "limine-uefi-cd.bin", "BOOTX64.EFI"]

KERNEL_CMDLINE = "console=ttyS0 console=tty0"
VOLUME_ID = "MAGNESIUM_A2"

LIMINE_CONF = f"""timeout: 3

/Magnesium OS v.A2
    protocol: linux
    path: boot():/boot/bzImage
    cmdline: {KERNEL_CMDLINE}
    module_path: boot():/boot/initramfs.gz
"""


def fetch_limine(cache_dir):
    """Return a directory holding the Limine files, downloading them if needed."""
    if all(os.path.exists(os.path.join(cache_dir, f)) for f in LIMINE_FILES):
        return cache_dir
    os.makedirs(cache_dir, exist_ok=True)
    print(f"Downloading Limine {LIMINE_COMMIT[:12]} ...")
    data = urllib.request.urlopen(LIMINE_URL, timeout=120).read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for name in LIMINE_FILES:
            matches = [m for m in tar.getmembers() if m.isfile() and os.path.basename(m.name) == name]
            if len(matches) != 1:
                sys.exit(f"error: expected one '{name}' in the Limine archive, found {len(matches)}")
            with tar.extractfile(matches[0]) as src, open(os.path.join(cache_dir, name), "wb") as dst:
                dst.write(src.read())
    return cache_dir


def build_iso(kernel, initrd, limine_dir, out):
    import pycdlib

    def read(path):
        with open(path, "rb") as f:
            return f.read()

    iso = pycdlib.PyCdlib()
    iso.new(joliet=True, rock_ridge="1.09", vol_ident=VOLUME_ID)

    # Directories (ISO 9660 names, Rock Ridge and Joliet names for the real ones).
    iso.add_directory("/BOOT", rr_name="boot", joliet_path="/boot")
    iso.add_directory("/BOOT/LIMINE", rr_name="limine", joliet_path="/boot/limine")
    iso.add_directory("/EFI", rr_name="EFI", joliet_path="/EFI")
    iso.add_directory("/EFI/BOOT", rr_name="BOOT", joliet_path="/EFI/BOOT")

    def add_file(data, iso_path, rr_name, joliet_path):
        iso.add_fp(io.BytesIO(data), len(data), iso_path=iso_path,
                   rr_name=rr_name, joliet_path=joliet_path)

    add_file(LIMINE_CONF.encode(), "/BOOT/LIMINE/LIMINE.CFG;1", "limine.conf", "/boot/limine/limine.conf")
    add_file(read(os.path.join(limine_dir, "limine-bios.sys")), "/BOOT/LIMINE/LIMINE_B.SYS;1",
             "limine-bios.sys", "/boot/limine/limine-bios.sys")
    add_file(read(os.path.join(limine_dir, "limine-bios-cd.bin")), "/BOOT/LIMINE/LIMINE_B.BIN;1",
             "limine-bios-cd.bin", "/boot/limine/limine-bios-cd.bin")
    add_file(read(os.path.join(limine_dir, "limine-uefi-cd.bin")), "/BOOT/LIMINE/LIMINE_U.BIN;1",
             "limine-uefi-cd.bin", "/boot/limine/limine-uefi-cd.bin")
    add_file(read(kernel), "/BOOT/BZIMAGE.;1", "bzImage", "/boot/bzImage")
    add_file(read(initrd), "/BOOT/INITRAM.GZ;1", "initramfs.gz", "/boot/initramfs.gz")
    add_file(read(os.path.join(limine_dir, "BOOTX64.EFI")), "/EFI/BOOT/BOOTX64.EFI;1",
             "BOOTX64.EFI", "/EFI/BOOT/BOOTX64.EFI")

    # BIOS entry (no-emulation, boot-info-table as Limine requires) and UEFI entry.
    iso.add_eltorito("/BOOT/LIMINE/LIMINE_B.BIN;1", bootcatfile="/BOOT.CAT;1",
                     boot_load_size=4, boot_info_table=True)
    iso.add_eltorito("/BOOT/LIMINE/LIMINE_U.BIN;1", bootcatfile="/BOOT.CAT;1", efi=True)

    iso.write(out)
    iso.close()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--kernel", default=os.path.join(here, "bzImage"))
    ap.add_argument("-i", "--initrd", default=os.path.join(here, "initramfs.gz"))
    ap.add_argument("-o", "--out", default=os.path.join(here, "magnesium-os-v.A2.iso"))
    ap.add_argument("--limine-cache", default=os.path.join(here, "build", "limine"))
    args = ap.parse_args()

    for p in (args.kernel, args.initrd):
        if not os.path.isfile(p):
            sys.exit(f"error: missing {p} (run ./build.sh first)")

    try:
        import pycdlib  # noqa: F401
    except ImportError:
        sys.exit("error: pycdlib is missing; install it with: pip install pycdlib")

    limine_dir = fetch_limine(args.limine_cache)
    build_iso(args.kernel, args.initrd, limine_dir, args.out)
    print(f"Wrote {args.out} ({os.path.getsize(args.out) // 1024} KiB)")


if __name__ == "__main__":
    main()
