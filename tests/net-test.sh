#!/usr/bin/env bash
# Boots Magnesium OS on a scratch copy of disk.img with a virtio-net NIC
# (QEMU user-mode networking), goes through first-start setup + login, then
# verifies the networking stack (drivers, DHCP lease, DNS) and the built-in
# /programs apps from the serial console.
# Needs: qemu-system-x86_64, bzImage, initramfs.gz, disk.img.
set -u
cd "$(dirname "$0")/.."
cp disk.img test-net-disk.img

LOG="boot-net.log"; QPID=""

vm_start() {
  : > "$LOG"
  rm -f vm.in; mkfifo vm.in
  exec 3<>vm.in       # keep the pipe open so QEMU never sees end-of-input
  timeout 300 qemu-system-x86_64 -kernel bzImage -initrd initramfs.gz \
    -append "console=ttyS0" -nographic -m 256M -no-reboot \
    -nic user,model=virtio-net-pci \
    -drive file=test-net-disk.img,if=virtio,format=raw < vm.in > "$LOG" 2>&1 &
  QPID=$!
}
vm_send() { printf '%s\r' "$1" >&3; }
vm_cmd()  { vm_send "$1"; sleep 4; }
vm_wait() {           # vm_wait PATTERN [COUNT] [SECONDS]
  local pat="$1" n="${2:-1}" t="${3:-120}" i=0
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
check() {             # check PATTERN DESCRIPTION
  if grep -q -- "$1" "$LOG"; then echo "PASS: $2"; else echo "FAIL: $2"; fails=$((fails + 1)); fi
}

vm_start

# First-start setup: root password, then a regular user, then log in as root.
vm_wait "New password: " 1 120 || { vm_stop; exit 1; }; vm_send rootpassword1
vm_wait "Retype password: " 1 || { vm_stop; exit 1; };    vm_send rootpassword1
vm_wait "Username (press Enter" 1 || { vm_stop; exit 1; }; vm_send tester
vm_wait "New password: " 2 || { vm_stop; exit 1; };       vm_send testerpassword1
vm_wait "Retype password: " 2 || { vm_stop; exit 1; };    vm_send testerpassword1
vm_wait "magnesium login: " 1 || { vm_stop; exit 1; };    vm_send root
vm_wait "Password: " 1 || { vm_stop; exit 1; };           vm_send rootpassword1
vm_wait "Welcome, root" 1 || { vm_stop; exit 1; };        sleep 3

# Networking: hardware/driver scan, DHCP lease from QEMU SLIRP, DNS lookup.
vm_cmd drivers
vm_cmd dhcp
vm_cmd ip
vm_cmd "dns example.com"

# Built-in apps in /programs: listed by ls and runnable by name.
vm_cmd "ls /programs"
vm_cmd "calc 2+2"
vm_cmd "sysmon --cli"

vm_send exit
vm_stop

check "virtio_net"      "virtio-net NIC driver detected"
check "10.0.2.15"       "DHCP lease acquired (10.0.2.15)"
check "example.com"     "DNS resolver answered"
check "netcenter"       "/programs/netcenter present"
check "mines"           "/programs/mines present"

echo "failures: $fails"
rm -f test-net-disk.img
exit "$fails"
