#!/usr/bin/env bash
# Boots ALXA OS twice on a scratch copy of disk.img, types into the console,
# and checks: first-start setup, login, permissions, and that accounts and files
# survive a reboot. Needs: qemu-system-x86_64, bzImage, initramfs.gz, disk.img.
set -u
cd "$(dirname "$0")/.."
cp disk.img test-disk.img

# vm LOGFILE LINE...  -> waits for boot, then types each line (3 s apart)
vm() {
  local log="$1"; shift
  ( sleep 25; for l in "$@"; do printf '%s\r' "$l"; sleep 4; done; sleep 2 ) |
    timeout 150 qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz \
      -append "console=ttyS0" -nographic -m 128M -no-reboot \
      -drive file=test-disk.img,if=virtio,format=raw > "$log" 2>&1
  return 0
}

fails=0
check() {  # check LOGFILE PATTERN DESCRIPTION
  if grep -q -- "$2" "$1"; then echo "PASS: $3"; else echo "FAIL: $3"; fails=$((fails + 1)); fi
}

echo "== boot 1: first start, accounts, permissions =="
vm boot1.log \
  rootpassword1 rootpassword1 tester testerpassword1 testerpassword1 \
  tester testerpassword1 whoami "cat /data/etc/shadow" "touch /bin/x" exit \
  root rootpassword1 "echo persisted > ~/note.txt" "cat ~/note.txt" id reboot
check boot1.log "First start"                 "first-start wizard appears"
check boot1.log "Disk mounted on /data"       "data disk is mounted"
check boot1.log "Welcome, tester"             "regular user can log in"
check boot1.log "Permission denied"           "regular user cannot read password hashes"
check boot1.log "Welcome, root"               "root can log in"
check boot1.log "uid=0(root)"                 "id shows root"
check boot1.log "persisted"                   "root can write into its home"

echo "== boot 2: everything must still be there =="
vm boot2.log root rootpassword1 "cat ~/note.txt" "ls /data" reboot
check boot2.log "Disk mounted on /data"       "disk mounted again"
check boot2.log "Welcome, root"               "account survived the reboot"
check boot2.log "persisted"                   "file survived the reboot"
if grep -q "First start" boot2.log; then echo "FAIL: first-start wizard ran again"; fails=$((fails + 1)); fi

echo "failures: $fails"
rm -f test-disk.img
exit $fails
