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

# Build the exact static SDK runtime that Windows installer CI embeds.
bash "$repo/tools/soil-ci/build-guest-cli.sh" "$tmp/soil"
ssh-keygen -q -t ed25519 -N "" -f "$tmp/guest" -C soil-guest-smoke

# Test the SAME in-guest preloading that the Windows installer ships, not
# an ephemeral copy over SSH. Only the dedicated Cua test supplies these.
cua_guest_args=()
if [[ -n "${SOIL_CUA_DRIVER_ARCHIVE:-}" ]]; then
  mkdir -p "$tmp/cua-release"
  tar -xzf "$SOIL_CUA_DRIVER_ARCHIVE" -C "$tmp/cua-release"
  driver_binary="$(find "$tmp/cua-release" -name cua-driver -type f -print -quit)"
  [[ -n "$driver_binary" && -s "$driver_binary" ]] || {
    echo "Pinned Cua release does not contain the Linux driver" >&2; exit 1;
  }
  [[ -s "${SOIL_CUA_DUMMY_MODULE:-}" && -s "${SOIL_CUA_DUMMY_LICENSE:-}" ]] || {
    echo "Real guest Cua test requires a signed Xorg dummy binary and license" >&2; exit 1;
  }
  cua_guest_args=(
    --cua-driver "$driver_binary"
    --cua-license "$repo/app/Soil/third-party/cua-driver-LICENSE.md"
    --xorg-dummy "$SOIL_CUA_DUMMY_MODULE"
    --xorg-license "$SOIL_CUA_DUMMY_LICENSE"
    --xorg-config "$repo/tools/soil-ci/xorg-dummy.conf"
    --desktop-ensure "$repo/tools/soil-ci/soil-ensure-desktop.sh"
  )
fi
python3 "$repo/tools/soil-ci/inject-native-guest.py" \
  "$images/core2.gz" "$tmp/core2-native.gz" \
  --launcher "$repo/soil-sdk/runtime/soil-native" --runtime "$tmp/soil" \
  "${cua_guest_args[@]}"
python3 "$repo/tools/soil-ci/inject-native-guest.py" \
  "$tmp/core2-native.gz" "$tmp/core2-native.gz" --runtime "$tmp/soil" \
  "${cua_guest_args[@]}" --verify
dotnet run --project "$repo/tools/soil-ci/GuestInitrdSmoke/GuestInitrdSmoke.csproj" -- \
  --prepare "$tmp/core2-native.gz" "$tmp/machine" "$tmp/guest.pub"
initrd="$tmp/machine/boot/core2-ssh.gz"
[[ -s "$initrd" ]] || { echo "SSH-enabled Linux guest image missing" >&2; exit 1; }

cat > "$tmp/native.c" <<'EOF'
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int packaged = argc > 1 && strcmp(argv[1], "--sol") == 0;
    const char *path = packaged ? "/tmp/soil-sol-guest-executed" :
                                     "/tmp/soil-native-guest-executed";
    int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0600);
    if (fd < 0) return 1;
    write(fd, "ok\n", 3);
    close(fd);
    if (!packaged) sleep(20);
    return 0;
}
EOF
gcc -static -O2 "$tmp/native.c" -o "$tmp/native-app"
python3 "$repo/soil-sdk/tools/soil.py" native "$tmp/native-app" \
  --id org.soil.smoke --name "CI Native SOL App" --arg=--sol -o "$tmp/NativeSmoke.sol"
python3 "$repo/soil-sdk/tools/soil.py" verify "$tmp/NativeSmoke.sol"

# Exercise a real third-party Linux application binary, not only our C fixture.
# busybox-static is supplied by Ubuntu in CI and is never shipped in the MSI.
busybox_elf="$(command -v busybox)"
[[ -f "$busybox_elf" ]] || { echo "busybox-static not installed" >&2; exit 1; }
mkdir -p "$tmp/BusyBoxDir"
cp "$busybox_elf" "$tmp/BusyBoxDir/busybox"
cat > "$tmp/BusyBoxDir/AppRun" <<'EOF'
#!/bin/sh
# BusyBox selects its applet from argv[0], so launch it under its own name.
exec "$APPDIR/app/busybox" echo soil-third-party-ok
EOF
chmod 755 "$tmp/BusyBoxDir/AppRun"
python3 "$repo/soil-sdk/tools/soil.py" native "$tmp/BusyBoxDir" \
  --id org.soil.busybox --name "BusyBox CI" \
  -o "$tmp/BusyBox.sol"
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

if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 "test -x /bin/soil-native && test -x /bin/soil && /bin/soil help >/dev/null"; then
  echo "Guest authenticated SSH, but native launcher or .sol runtime is missing." >&2
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "ls -l /bin/soil /bin/soil-native /usr/local/bin/soil 2>&1; mount | grep -E '/usr/local|/bin' || true" >&2 || true
  exit 1
fi

cat "$tmp/native-app" | ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat > /tmp/soil-ci-native"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native install nativeapp /tmp/soil-ci-native 'CI Native Linux App'"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native launch nativeapp && sleep 1 && test -f /tmp/soil-native-guest-executed"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native list | grep '^nativeapp$' && soil-native remove nativeapp"
# A real guest startup failure must be visible to the host/agent instead of
# silently reporting 'launched'. Simulate an ELF with a missing dependency.
cat > "$tmp/failing.c" <<'EOF'
#include <stdio.h>
int main(void) {
    fputs("soil-smoke-expected-startup-error: missing dependency\\n", stderr);
    return 127;
}
EOF
gcc -static -O2 "$tmp/failing.c" -o "$tmp/failing-app"
cat "$tmp/failing-app" | ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat > /tmp/soil-ci-failing"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native install failapp /tmp/soil-ci-failing 'CI Failure'"
if ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native launch failapp" >"$tmp/launch-error.log" 2>&1; then
  echo "Guest incorrectly reported a failed app as launched" >&2
  exit 1
fi
grep -q "soil-smoke-expected-startup-error" "$tmp/launch-error.log" || {
  echo "Guest startup error was not relayed to host" >&2
  cat "$tmp/launch-error.log" >&2
  exit 1
}
ssh "${ssh_opts[@]}" tc@127.0.0.1 "soil-native remove failapp"


# Transfer a genuinely packaged native .sol (header, manifest, SquashFS, digest).
# It must mount and execute in the actual guest kernel, not on the CI host.
cat "$tmp/NativeSmoke.sol" | ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat > /tmp/NativeSmoke.sol"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil info /tmp/NativeSmoke.sol | grep native"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil verify /tmp/NativeSmoke.sol"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil run /tmp/NativeSmoke.sol && test -f /tmp/soil-sol-guest-executed"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "rm /tmp/soil-sol-guest-executed && /bin/soil install /tmp/NativeSmoke.sol"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "grep '^Exec=/bin/soil launch org.soil.smoke$' ~/.local/share/applications/soil-org.soil.smoke.desktop"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil launch org.soil.smoke && test -f /tmp/soil-sol-guest-executed"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil list | grep org.soil.smoke && /bin/soil remove org.soil.smoke"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "! /bin/soil list | grep -q org.soil.smoke"
cat "$tmp/BusyBox.sol" | ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat > /tmp/BusyBox.sol"
ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil run /tmp/BusyBox.sol | grep -x soil-third-party-ok"
echo "PASS: real bundled Soil guest boot; raw ELF, native .sol lifecycle, and third-party BusyBox binary."

if [[ -n "${SOIL_CUA_DRIVER_ARCHIVE:-}" ]]; then
  echo "PROBE: actual preloaded upstream Cua Driver and tested Xorg dummy"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "ls -l /bin/cua-driver /bin/soil-ensure-desktop \
     /bin/soil-xorg/drivers/dummy_drv.so /etc/X11/soil-dummy.conf \
     /usr/share/doc/cua-driver/LICENSE.md \
     /usr/share/doc/xf86-video-dummy/copyright 2>&1 || true"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "test -x /bin/cua-driver && test -x /bin/soil-ensure-desktop && \
     test -f /bin/soil-xorg/drivers/dummy_drv.so && \
     test -s /usr/share/doc/cua-driver/LICENSE.md && \
     test -s /usr/share/doc/xf86-video-dummy/copyright"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/cua-driver --version"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/cua-driver telemetry disable"
  # Before patching the guest this was consistently broken: /proc/fb was empty,
  # /dev/fb0 had no driver, and Xvfb crashed. The bundled Xorg dummy now
  # supplies the real guest desktop without needing any extra kernel module.
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "/bin/soil-ensure-desktop"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 xset q >/dev/null 2>&1 && ps w | grep -q '[f]luxbox'"
  echo "PROBE: X11 server is reachable on DISPLAY=:0"
  # Start Cua only after X is usable. The daemon initializes its capture
  # backend at startup and does not necessarily recover from a missing X socket.
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 nohup /bin/cua-driver serve >/tmp/cua-driver.log 2>&1 </dev/null &"
  # First-run initialization takes variable time. Poll the actual socket,
  # do not assume an arbitrary two- or three-second delay is sufficient.
  driver_ready=0
  for attempt in $(seq 1 30); do
    if ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "DISPLAY=:0 /bin/cua-driver status >/dev/null 2>&1"; then
      driver_ready=1
      break
    fi
    sleep 1
  done
  if [[ "$driver_ready" != 1 ]]; then
    ssh "${ssh_opts[@]}" tc@127.0.0.1 "tail -n 70 /tmp/cua-driver.log" || true
    echo "Cua Driver did not serve inside the guest" >&2
    exit 1
  fi
  if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 /bin/cua-driver call get_desktop_state > /tmp/cua-desktop-state.json"; then
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "tail -n 70 /tmp/cua-driver.log; head -c 1200 /tmp/cua-desktop-state.json" || true
    echo "Cua Driver cannot observe the actual VM desktop" >&2
    exit 1
  fi
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "test -s /tmp/cua-desktop-state.json && wc -c /tmp/cua-desktop-state.json"
  # Verify actual Cua mouse actions against the same observed desktop.
  # The Xorg dummy driver uses exact X11 coordinates, not Soil's relative PS/2
  # fallback. The action CLI returns a structured effect/result.
  mouse_args='{"target":{"kind":"desktop","display_id":"primary"},"x":180,"y":125}'
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 /bin/cua-driver call move_cursor '$mouse_args' >/tmp/cua-move.json"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 /bin/cua-driver call get_cursor_position > /tmp/cua-cursor.json"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 /bin/cua-driver call click '$mouse_args' >/tmp/cua-click.json"
  for name in move cursor click; do
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "test -s /tmp/cua-$name.json && head -c 350 /tmp/cua-$name.json && echo"
  done
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "cat /tmp/cua-desktop-state.json" \
    > "$tmp/cua-desktop-state.json"
  python3 - "$tmp/cua-desktop-state.json" <<'PY'
import json, sys
payload = json.load(open(sys.argv[1], encoding="utf-8"))
print("Cua screenshot response keys:", list(payload) if isinstance(payload, dict) else type(payload).__name__)
if isinstance(payload, dict) and payload.get("isError") is True:
    raise SystemExit("Cua Driver returned an error instead of a desktop screenshot")
PY
  echo "PASS: upstream Cua Driver desktop capture, mouse move and click executed in real guest."
fi

