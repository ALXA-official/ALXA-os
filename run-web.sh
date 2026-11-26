#!/usr/bin/env bash
# Runs Magnesium OS inside a web browser (GitHub Codespaces, Gitpod, Cloud Shell,
# or any Linux machine) using QEMU + noVNC on port 6080.
#
# Usage:
#   ./run-web.sh              # VirtIO-GPU KMS driver mode (default)
#   GPU=basic ./run-web.sh    # Basic Display / no-GPU-driver mode (VESA simpledrm)
#   GPU=std   ./run-web.sh    # QEMU Standard VGA (bochs-drm KMS)
set -euo pipefail

GPU="${GPU:-virtio}"
PORT="${PORT:-6080}"
VNC_DISPLAY="${VNC_DISPLAY:-0}"
VNC_PORT=$((5900 + VNC_DISPLAY))
MEM="${MEM:-256M}"

# 1) Ensure QEMU + noVNC + websockify are installed
if ! command -v qemu-system-x86_64 >/dev/null 2>&1 || \
   ! command -v websockify >/dev/null 2>&1 || \
   [ ! -d /usr/share/novnc ]; then
  echo "Installing QEMU + noVNC browser viewer..."
  sudo apt-get update
  sudo apt-get install -y qemu-system-x86 novnc websockify
fi

# 2) Ensure bzImage + initramfs.gz exist (download latest CI build if available, else build locally)
if [ ! -f bzImage ] || [ ! -f initramfs.gz ]; then
  if command -v gh >/dev/null 2>&1; then
    echo "Checking GitHub Actions for a pre-built magnesium-os artifact..."
    gh run download --name magnesium-os 2>/dev/null || true
  fi
fi
if [ ! -f bzImage ] || [ ! -f initramfs.gz ]; then
  echo "No pre-built image found locally; building Magnesium OS now..."
  ./build.sh
fi
if [ ! -f disk.img ] && command -v mke2fs >/dev/null 2>&1; then
  truncate -s 32M disk.img
  mke2fs -q -t ext2 -F -L MAGNESIUMDATA -E root_owner=0:0 disk.img
fi

# 3) Prepare a static web directory that redirects "/" straight to the live auto-connecting screen
WEBDIR="$(mktemp -d /tmp/mg-novnc.XXXXXX)"
cp -a /usr/share/novnc/. "$WEBDIR/"
cat > "$WEBDIR/index.html" << 'EOF'
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="refresh" content="0; url=vnc.html?autoconnect=true&resize=scale">
  <title>Magnesium OS</title>
</head>
<body style="background:#0a1324;color:#dce7f7;font-family:sans-serif;padding:2rem">
  Opening Magnesium OS... <a style="color:#5eb8ff" href="vnc.html?autoconnect=true&resize=scale">Click here</a> if not redirected.
</body>
</html>
EOF

cleanup() {
  [ -n "${WS_PID:-}" ] && kill "$WS_PID" 2>/dev/null || true
  rm -rf "$WEBDIR"
}
trap cleanup EXIT INT TERM

websockify --web "$WEBDIR" "0.0.0.0:${PORT}" "127.0.0.1:${VNC_PORT}" >/dev/null 2>&1 &
WS_PID=$!

echo ""
echo "================================================================"
echo "  Magnesium OS is live in your browser on port ${PORT}!"
if [ -n "${CODESPACE_NAME:-}" ] && [ -n "${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN:-}" ]; then
  echo "  Open: https://${CODESPACE_NAME}-${PORT}.${GITHUB_CODESPACES_PORT_FORWARDING_DOMAIN}/"
else
  echo "  Open: http://localhost:${PORT}/"
fi
echo "  (Display mode: GPU=${GPU} | Press Ctrl+C here to stop)"
echo "================================================================"
echo ""

DISK=()
[ -f disk.img ] && DISK=(-drive file=disk.img,if=virtio,format=raw)

KVM=()
[ -w /dev/kvm ] && KVM=(-enable-kvm -cpu host)

case "$GPU" in
  basic|none)
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "vga=792 nomodeset console=ttyS0 console=tty0" \
      -vga std -usb -device usb-tablet \
      -vnc "127.0.0.1:${VNC_DISPLAY}" -serial stdio "${DISK[@]}"
    ;;
  std|bochs)
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0 console=tty0" \
      -vga std -usb -device usb-tablet \
      -vnc "127.0.0.1:${VNC_DISPLAY}" -serial stdio "${DISK[@]}"
    ;;
  virtio|*)
    exec qemu-system-x86_64 "${KVM[@]}" -smp 2 -m "$MEM" \
      -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0 console=tty0" \
      -vga none -device virtio-gpu-pci,xres=1280,yres=800 \
      -usb -device usb-tablet \
      -vnc "127.0.0.1:${VNC_DISPLAY}" -serial stdio "${DISK[@]}"
    ;;
esac
