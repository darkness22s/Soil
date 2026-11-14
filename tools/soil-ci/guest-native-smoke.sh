#!/usr/bin/env bash
# Boot Soil's ACTUAL bundled Linux guest image in QEMU on Linux CI.
# This tests the same vmlinuz64/core2.gz/soil.squashfs boot payload
# used by the default Windows WHPX VM, not the Linux-hosted Soil launcher.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
images="${1:-$repo/app/images}"
tmp="$(mktemp -d -t soil-native-vm-XXXXXX)"
qemu_pid=""
cleanup() {
  if [[ -n "$qemu_pid" ]]; then
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT

for tool in python3 dotnet qemu-system-x86_64 ssh ssh-keygen gcc; do
  command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done
for file in vmlinuz64 core2.gz soil.squashfs; do
  [[ -s "$images/$file" && $(stat -c %s "$images/$file") -gt 1000000 ]] ||
    { echo "Actual private LFS image $file missing, refusing fake boot" >&2; exit 1; }
done

ssh-keygen -q -t ed25519 -N "" -f "$tmp/guest" -C soil-guest-smoke
python3 "$repo/tools/soil-ci/inject-native-guest.py" \
  "$images/core2.gz" "$tmp/core2-native.gz" \
  --launcher "$repo/soil-sdk/runtime/soil-native"
dotnet run --project "$repo/tools/soil-ci/GuestInitrdSmoke/GuestInitrdSmoke.csproj" -- \
  --prepare "$tmp/core2-native.gz" "$tmp/machine" "$tmp/guest.pub"
initrd="$tmp/machine/boot/core2-ssh.gz"
[[ -s "$initrd" ]] || { echo "SSH-enabled Linux guest image missing" >&2; exit 1; }

cat > "$tmp/native.c" <<'EOF'
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    int fd = open("/tmp/soil-native-guest-executed", O_CREAT|O_TRUNC|O_WRONLY, 0600);
    if (fd < 0) return 1;
    write(fd, "ok\n", 3);
    close(fd);
    sleep(20);
    return 0;
}
EOF
gcc -static -O2 "$tmp/native.c" -o "$tmp/native-app"
truncate -s 10G "$tmp/data.img"

qemu-system-x86_64 \
  -machine q35 -accel tcg,thread=multi -cpu max -smp 2 -m 2048 \
  -kernel "$images/vmlinuz64" -initrd "$initrd" \
  -append "console=ttyS0 quiet loglevel=3 acpi=off noapic nolapic mitigations=off nozswap" \
  -drive file="$images/soil.squashfs",format=raw,if=virtio,readonly=on \
  -drive file="$tmp/data.img",format=raw,if=virtio \
  -netdev user,id=soilnet,hostfwd=tcp:127.0.0.1:22822-:22 \
  -device virtio-net-pci,netdev=soilnet \
  -vga std -display none -serial file:"$tmp/serial.log" \
  -monitor none -no-reboot >"$tmp/qemu.log" 2>&1 &
qemu_pid=$!

ssh_opts=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1
          -i "$tmp/guest" -p 22822)
reachable=0
for attempt in $(seq 1 90); do
  if ! kill -0 "$qemu_pid" 2>/dev/null; then
    echo "Soil guest QEMU exited prematurely" >&2
    break
  fi
  if ssh "${ssh_opts[@]}" tc@127.0.0.1 "id -un" >/dev/null 2>&1; then
    reachable=1
    break
  fi
  sleep 2
done
if [[ "$reachable" != 1 ]]; then
  echo "Real Soil guest boot did not provide authenticated SSH and soil-native" >&2
  echo "SSH failure diagnostics:" >&2
  timeout 8 ssh -vvv "${ssh_opts[@]}" tc@127.0.0.1 "id -un" >&2 || true
  echo "Guest networking and SSH boot diagnostics:" >&2
  grep -Ei 'eth|dhcp|network|dropbear|ssh|pubkey' "$tmp/serial.log" | tail -n 60 >&2 || true
  tail -n 100 "$tmp/serial.log" >&2 || true
  tail -n 100 "$tmp/qemu.log" >&2 || true
  exit 1
fi

if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 "test -x /bin/soil-native && command -v soil-native"; then
  echo "Guest authenticated SSH, but native launcher is missing from rootfs." >&2
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "ls -l /bin/soil-native /usr/bin/soil-native /usr/local/bin/soil-native 2>&1; mount | grep -E '/usr/local|/bin' || true" >&2 || true
  exit 1
fi

cat "$tmp/native-app" | ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat > /tmp/soil-ci-native"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native install nativeapp /tmp/soil-ci-native 'CI Native Linux App'"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native launch nativeapp && sleep 1 && test -f /tmp/soil-native-guest-executed"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native list | grep '^nativeapp$' && soil-native remove nativeapp"
echo "PASS: real default Soil Linux guest boot, SSH transfer, native x86-64 ELF execution, removal."
