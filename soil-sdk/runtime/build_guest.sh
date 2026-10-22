#!/bin/sh
# Build soil + soil-app-bin inside the Tiny Core dev farm (run as root).
set -e
echo SOIL-BUILD-START
free -m
mkdir -p /home/tc/dl/dev /home/tc/src /home/tc/out /tmp/tcloop
umount /tmp/tcloop/* 2>/dev/null
rm -rf /tmp/tcloop/* 2>/dev/null || true
cd /home/tc/dl/dev
for n in compiletc bison diffutils file findutils gawk gcc gcc_base-dev gcc_libs-dev grep m4 make patch pkgconf e2fsprogs_base-dev glibc_base-dev linux-6.12_api_headers util-linux_base-dev zlib_base-dev glibc_add_lib libxcrypt_base-dev gtk3-dev gdk-pixbuf2-dev at-spi2-core-dev pango-dev wayland-dev rest-dev colord-dev json-glib-dev libxkbcommon-dev libXcursor-dev libXcomposite-dev libXinerama-dev libepoxy-dev cairo-dev libXft-dev fribidi-dev harfbuzz-dev harfbuzz-bin graphite-dev harfbuzz-icu-dev glib2-dev pixman-dev freetype-dev fontconfig-dev libpng-dev libjpeg-turbo-dev libX11-dev libxcb-dev libXrender-dev libXext-dev libXau-dev libXdmcp-dev libXfixes-dev libXrandr-dev libXdamage-dev libXi-dev webkitgtk-gtk3-dev libsoup3-dev libxml2-dev sqlite3-dev libpsl-dev libnghttp2-dev libsecret-dev gudev-lib-dev libxslt-dev openjpeg-dev mpfr mpc isl libnotify libnotify-dev binutils dbus dbus-dev libXtst libXtst-dev pcre2 pcre2-dev libffi libffi-dev expat2 expat2-dev libtiff xtrans; do
  [ -s $n.tcz ] || wget -q -O $n.tcz http://10.0.2.2:8810/$n.tcz || echo DEV-DL-FAIL:$n
done
for n in *.tcz; do
  e=${n%.tcz}
  mkdir -p /tmp/tcloop/$e
  mount -t squashfs -o ro,loop /home/tc/dl/dev/$n /tmp/tcloop/$e 2>/dev/null || echo DEV-MNT-FAIL:$e
done
link_tree() {
  _tree="$1"; _prefix="$2"
  ( cd "$_tree" 2>/dev/null && find . \( -type f -o -type l \) 2>/dev/null | while read f; do
      t=$_prefix/${f#./}
      if [ -e "$t" ] || [ -L "$t" ]; then continue; fi
      mkdir -p "$(dirname "$t")" 2>/dev/null
      ln -s "$_tree/${f#./}" "$t" 2>/dev/null
    done )
}
for tree in /tmp/tcloop/*/usr/local; do
  [ -d "$tree" ] || continue
  link_tree "$tree" /usr/local
done
for tree in /tmp/tcloop/*/usr; do
  [ -d "$tree" ] || continue
  for sub in include lib bin share; do
    [ -d "$tree/$sub" ] || continue
    link_tree "$tree/$sub" "/usr/$sub"
  done
done
echo FARM-LINKED
PCS=/usr/local/lib/pkgconfig
mkdir -p $PCS
for f in zlib libtiff-4 libpcre2-8 gl egl xproto kbproto xextproto renderproto randrproto xineramaproto damageproto fixesproto recordproto xcmiscproto bigreqsproto resourceproto scrnsaverproto compositeproto inputproto; do rm -f $PCS/$f.pc; done
cat > $PCS/zlib.pc <<'EOF'
prefix=/usr/local
Name: zlib
Description: zlib compression
Version: 1.3.1
Libs: -lz
Cflags:
EOF
cat > $PCS/libtiff-4.pc <<'EOF'
prefix=/usr/local
Name: libtiff-4
Description: TIFF stub
Version: 4.7.0
Libs:
Cflags:
EOF
cat > $PCS/libpcre2-8.pc <<'EOF'
prefix=/usr/local
Name: libpcre2-8
Description: pcre2 8-bit
Version: 10.45
Libs: -L/usr/local/lib -lpcre2-8
Cflags:
EOF
cat > $PCS/gl.pc <<'EOF'
Name: gl
Description: OpenGL runtime stub
Version: 1.0
Libs: -lGL
Cflags:
EOF
cat > $PCS/egl.pc <<'EOF'
Name: egl
Description: EGL runtime stub
Version: 1.0
Libs: -lEGL
Cflags:
EOF
for p in xproto kbproto xextproto renderproto randrproto xineramaproto damageproto fixesproto recordproto xcmiscproto bigreqsproto resourceproto scrnsaverproto compositeproto inputproto; do
cat > $PCS/$p.pc <<EOF
Name: $p
Description: $p stub
Version: 7.0.0
EOF
done
export PKG_CONFIG_PATH=$PCS:/usr/local/share/pkgconfig:$(for d in /tmp/tcloop/*/usr/local/lib/pkgconfig /tmp/tcloop/*/usr/lib/pkgconfig /tmp/tcloop/*/usr/local/share/pkgconfig; do printf "%s:" $d; done)
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LD_LIBRARY_PATH=$(for d in /tmp/tcloop/*/usr/local/lib /tmp/tcloop/*/usr/lib; do printf "%s:" $d; done)
PKGC=pkgconf
command -v pkgconf >/dev/null 2>&1 || PKGC=pkg-config
cd /home/tc/src
wget -q -O src.tar.gz http://10.0.2.2:8808/soil-runtime-src.tar.gz || echo SRC-DL-FAIL
tar xzf src.tar.gz
gcc --version 2>&1 | head -1
echo CORE-BUILD
gcc -O2 -Wall -Wno-format-truncation -o /home/tc/out/soil soil.c sol.c json.c sha256.c 2>&1 | head -60 || echo CORE-BUILD-FAIL
if [ -f /home/tc/out/soil ]; then echo CORE-BUILD-OK; fi
echo APP-BUILD
gcc -O2 -Wl,--allow-shlib-undefined -o /home/tc/out/soil-app-bin soil-app.c json.c $($PKGC --cflags --libs webkit2gtk-4.1 libnotify) 2>&1 | head -80 || echo APP-BUILD-FAIL
if [ -f /home/tc/out/soil-app-bin ]; then echo APP-BUILD-OK; fi
ls -la /home/tc/out
echo SOIL-BUILD-DONE
free -m
