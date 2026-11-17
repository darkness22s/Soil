#!/bin/sh
# Soil's optional local Xorg software desktop. The stock Tiny Core kernel
# may expose a /dev/fb0 node with no registered framebuffer; this uses the
# existing Xorg server and a bundled open-source dummy video driver instead.
# Called only when a VM's desktop is actually needed, never from the host
# background or while the VM is parked.
set -eu
export DISPLAY=:0
if xset q >/dev/null 2>&1; then
    exit 0
fi

if [ ! -f /bin/soil-xorg/drivers/dummy_drv.so ] ||
   [ ! -f /etc/X11/soil-dummy.conf ] ||
   [ ! -x /usr/local/bin/Xorg ]; then
    echo "Soil desktop: Xorg virtual-display runtime missing from guest image" >&2
    exit 1
fi

mkdir -p /tmp/soil
if mkdir /tmp/soil/desktop-starting 2>/dev/null; then
    trap 'rmdir /tmp/soil/desktop-starting 2>/dev/null || true' EXIT HUP INT TERM
    if ! xset q >/dev/null 2>&1; then
        # Launch the shipped Xorg server with no TCP listener. All desktop apps
        # and the official Cua Driver share this exact X11 session.
        sudo -n sh -c 'nohup /usr/local/bin/Xorg :0 \
          -config /etc/X11/soil-dummy.conf \
          -modulepath /bin/soil-xorg,/usr/local/lib/xorg/modules \
          -noreset -nolisten tcp -ac -logfile /tmp/soil-xorg.log \
          -novtswitch >/tmp/soil-xorg-stdout.log 2>&1 </dev/null &'
    fi
fi

i=0
while [ "$i" -lt 25 ]; do
    if xset q >/dev/null 2>&1; then
        if ! ps w | grep -q '[f]luxbox'; then
            (nohup fluxbox >/tmp/soil-fluxbox.log 2>&1 </dev/null &) || true
        fi
        if ! ps w | grep -q '[p]cmanfm.*--desktop'; then
            (nohup pcmanfm --desktop >/tmp/soil-pcmanfm.log 2>&1 </dev/null &) || true
        fi
        echo "Soil desktop: X11 ready on :0"
        exit 0
    fi
    sleep 1
    i=$((i + 1))
done
echo "Soil desktop: X11 startup failed; inspect /tmp/soil-xorg.log" >&2
exit 1
