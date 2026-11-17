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
python3 "$repo/tools/soil-ci/inject-native-guest.py" \
  "$images/core2.gz" "$tmp/core2-native.gz" \
  --launcher "$repo/soil-sdk/runtime/soil-native" --runtime "$tmp/soil"
python3 "$repo/tools/soil-ci/inject-native-guest.py" \
  "$tmp/core2-native.gz" "$tmp/core2-native.gz" --runtime "$tmp/soil" --verify
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
  echo "PROBE: verified upstream Cua Driver inside the actual Soil Linux VM"
  mkdir -p "$tmp/cua-driver"
  tar -xzf "$SOIL_CUA_DRIVER_ARCHIVE" -C "$tmp/cua-driver"
  driver_binary="$(find "$tmp/cua-driver" -type f -name cua-driver -print -quit)"
  [[ -n "$driver_binary" ]] || { echo "Upstream Cua release has no driver binary" >&2; exit 1; }
  file "$driver_binary" || true
  cat "$driver_binary" | ssh "${ssh_opts[@]}" tc@127.0.0.1     "cat > /tmp/cua-driver && chmod 755 /tmp/cua-driver"
  ssh "${ssh_opts[@]}" tc@127.0.0.1     "getconf GNU_LIBC_VERSION; printf 'DISPLAY=%s\\n' \"\$DISPLAY\"; ldd /tmp/cua-driver || true"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "DISPLAY=:0 /tmp/cua-driver --version"
  # Guest images must not send telemetry. Disable before starting the daemon.
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "DISPLAY=:0 /tmp/cua-driver telemetry disable"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 "DISPLAY=:0 /tmp/cua-driver doctor"
  echo "PROBE: kernel framebuffer, X modules, and available alternative servers"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "cat /proc/fb 2>&1 || true; cat /proc/cmdline; ls -la /sys/class/graphics /dev/fb* 2>&1 || true; \
     cat /proc/modules | grep -E 'fb|drm|video' || true; \
     find /usr/local/lib/xorg/modules -type f -name '*drv.so' 2>/dev/null || true; \
     for bin in Xvnc Xtigervnc Xorg Xfbdev Xvfb Xephyr Xwayland modprobe; do \
       command -v \$bin || true; done; \
     ls /lib/modules/\$(uname -r)/kernel/drivers/video/fbdev 2>&1 || true" || true
  echo "PROBE: actual guest desktop/server diagnostics"
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "ps w; ls -la /tmp/.X11-unix /tmp/.X0-lock /dev/fb0 2>&1 || true; \
     for bin in Xorg Xvesa Xfbdev Xvfb startx xinit dbus-daemon; do \
       command -v \$bin || true; done; \
     ls /usr/local/etc/X11 /home/tc/.xsession /home/tc/.xinitrc 2>&1 || true" || true
  # The direct kernel boot has no kernel fb0 device and the default serial
  # console does not start X. Cua works against an isolated virtual display.
  if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 "DISPLAY=:0 xset q >/dev/null 2>&1"; then
    echo "PROBE: starting guest Xvfb display for genuine Cua Driver"
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "nohup Xvfb :0 -screen 0 1360x768x24 -nolisten tcp >/tmp/soil-xvfb.log 2>&1 </dev/null &" || true
    gui_ready=0
    for attempt in $(seq 1 15); do
      if ssh "${ssh_opts[@]}" tc@127.0.0.1 \
        "DISPLAY=:0 xset q >/dev/null 2>&1"; then gui_ready=1; break; fi
      sleep 1
    done
    if [[ "$gui_ready" != 1 ]]; then
      ssh "${ssh_opts[@]}" tc@127.0.0.1 \
        "tail -n 100 /tmp/soil-xvfb.log; ls -l /tmp/.X11-unix; ps w | grep Xvfb || true" || true
      echo "Virtual X11 display failed to start inside guest" >&2
      exit 1
    fi
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "DISPLAY=:0 nohup fluxbox >/tmp/soil-fluxbox.log 2>&1 </dev/null &" || true
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "DISPLAY=:0 nohup pcmanfm --desktop >/tmp/soil-pcmanfm.log 2>&1 </dev/null &" || true
  fi
  echo "PROBE: X11 server is reachable on DISPLAY=:0"
  # Start Cua only after X is usable. The daemon initializes its capture
  # backend at startup and does not necessarily recover from a missing X socket.
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 nohup /tmp/cua-driver serve >/tmp/cua-driver.log 2>&1 </dev/null &"
  sleep 3
  if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 "DISPLAY=:0 /tmp/cua-driver status"; then
    ssh "${ssh_opts[@]}" tc@127.0.0.1 "tail -n 70 /tmp/cua-driver.log" || true
    echo "Cua Driver did not serve inside the guest" >&2
    exit 1
  fi
  if ! ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "DISPLAY=:0 /tmp/cua-driver call get_desktop_state > /tmp/cua-desktop-state.json"; then
    ssh "${ssh_opts[@]}" tc@127.0.0.1 \
      "tail -n 70 /tmp/cua-driver.log; head -c 1200 /tmp/cua-desktop-state.json" || true
    echo "Cua Driver cannot observe the actual VM desktop" >&2
    exit 1
  fi
  ssh "${ssh_opts[@]}" tc@127.0.0.1 \
    "test -s /tmp/cua-desktop-state.json && wc -c /tmp/cua-desktop-state.json"
  echo "PROBE: upstream Cua Driver daemon and desktop observation executed in guest."
fi

