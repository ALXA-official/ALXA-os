#!/usr/bin/env bash
# Boots Magnesium OS twice on a scratch copy of disk.img and drives the console like a
# person would: it waits for each prompt before typing. Checks first-start setup,
# login, permissions, and that accounts and files survive a reboot.
# Needs: qemu-system-x86_64, bzImage, initramfs.gz, disk.img.
set -u
cd "$(dirname "$0")/.."
cp disk.img test-disk.img

LOG=""; QPID=""

vm_start() {          # vm_start LOGFILE
  LOG="$1"; : > "$LOG"
  rm -f vm.in; mkfifo vm.in
  exec 3<>vm.in       # keep the pipe open so QEMU never sees end-of-input
  timeout 300 qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz \
    -append "console=ttyS0" -nographic -m 128M -no-reboot \
    -drive file=test-disk.img,if=virtio,format=raw < vm.in > "$LOG" 2>&1 &
  QPID=$!
}
vm_send() { printf '%s\r' "$1" >&3; }
vm_cmd()  { vm_send "$1"; sleep 3; }
vm_wait() {           # vm_wait PATTERN [COUNT] [SECONDS]: PATTERN must appear on COUNT lines
  local pat="$1" n="${2:-1}" t="${3:-90}" i=0
  while [ "$(grep -c -- "$pat" "$LOG" 2>/dev/null)" -lt "$n" ]; do
    sleep 1; i=$((i + 1))
    if [ "$i" -ge "$t" ]; then echo "TIMEOUT waiting for: $pat"; return 1; fi
    kill -0 "$QPID" 2>/dev/null || { echo "VM stopped while waiting for: $pat"; return 1; }
  done
}
vm_stop() {
  sleep 4
  kill "$QPID" 2>/dev/null
  wait "$QPID" 2>/dev/null
  exec 3>&-
  rm -f vm.in
}

fails=0
check() {             # check LOGFILE PATTERN DESCRIPTION
  if grep -q -- "$2" "$1"; then echo "PASS: $3"; else echo "FAIL: $3"; fails=$((fails + 1)); fi
}

boot1() {
  vm_wait "New password: " 1 120 || return 1;   vm_send rootpassword1
  vm_wait "Retype password: " 1 || return 1;    vm_send rootpassword1
  vm_wait "Username (press Enter" 1 || return 1; vm_send tester
  vm_wait "New password: " 2 || return 1;       vm_send testerpassword1
  vm_wait "Retype password: " 2 || return 1;    vm_send testerpassword1
  vm_wait "magnesium login: " 1 || return 1;         vm_send tester
  vm_wait "Password: " 1 || return 1;           vm_send testerpassword1
  vm_wait "Welcome, tester" 1 || return 1;      sleep 3
  vm_cmd whoami
  vm_cmd "cat /data/etc/shadow"
  vm_cmd "touch /bin/x"
  vm_cmd exit
  vm_wait "magnesium login: " 2 || return 1;         vm_send root
  vm_wait "Password: " 2 || return 1;           vm_send rootpassword1
  vm_wait "Welcome, root" 1 || return 1;        sleep 3
  vm_cmd "echo persisted > ~/note.txt"
  vm_cmd "cat ~/note.txt"
  vm_cmd id
  vm_send reboot
}

boot2() {
  vm_wait "magnesium login: " 1 120 || return 1;     vm_send root
  vm_wait "Password: " 1 || return 1;           vm_send rootpassword1
  vm_wait "Welcome, root" 1 || return 1;        sleep 3
  vm_cmd "cat ~/note.txt"
  vm_cmd "ls /data"
  vm_send reboot
}

echo "== boot 1: first start, accounts, permissions =="
vm_start boot1.log; boot1; vm_stop
check boot1.log "First start"            "first-start setup appears"
check boot1.log "Disk mounted on /data"  "data disk is mounted"
check boot1.log "Welcome, tester"        "regular user can log in"
check boot1.log "Permission denied"      "regular user is blocked from protected files"
check boot1.log "Welcome, root"          "root can log in"
check boot1.log "uid=0(root)"            "id shows root"
check boot1.log "persisted"              "root can write into its home"

echo "== boot 2: everything must still be there =="
vm_start boot2.log; boot2; vm_stop
check boot2.log "Disk mounted on /data"  "disk mounted again"
check boot2.log "Welcome, root"          "account survived the reboot"
check boot2.log "persisted"              "file survived the reboot"
if grep -q "First start" boot2.log; then echo "FAIL: first-start setup ran again"; fails=$((fails + 1)); fi

echo "failures: $fails"
rm -f test-disk.img
exit "$fails"
