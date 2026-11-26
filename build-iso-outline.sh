#!/bin/bash
# build-iso-outline.sh
# Outline for packaging LiteBird into a minimal bootable ISO usable with copy.sh/v86
# Run this on a host that can do i386 debootstrap (install qemu-user-static if needed).
# This is a high-level guide; adjust paths and packages as needed.

# FIX: also fail on unset variables and on errors inside pipelines.
set -euo pipefail
ROOT=litebird-root
ISO=litebird-ubuntu-i386.iso   # used by the live-build step below
: "$ISO"

# FIX: fail early with a clear message instead of after a long debootstrap.
if [ ! -x ./litebird ]; then
    echo "error: ./litebird not found. Build a 32-bit (i386) binary first (see v86-notes.md)." >&2
    exit 1
fi
if command -v file >/dev/null && ! file ./litebird | grep -q "80386"; then
    echo "warning: ./litebird does not look like an i386 binary; v86 only runs 32-bit x86." >&2
fi

echo "=== 1. Create minimal i386 Ubuntu rootfs (Bionic 18.04) ==="
sudo debootstrap --arch=i386 --variant=minbase bionic "$ROOT" http://archive.ubuntu.com/ubuntu

echo "=== 2. Prepare chroot ==="
sudo cp /usr/bin/qemu-i386-static "$ROOT/usr/bin/" 2>/dev/null || true
# FIX: openbox and libfltk1.3 live in "universe", which debootstrap does not enable.
echo "deb http://archive.ubuntu.com/ubuntu bionic main universe" | sudo tee "$ROOT/etc/apt/sources.list" >/dev/null
# FIX: apt/dpkg maintainer scripts need /proc, /sys and /dev inside the chroot.
for d in proc sys dev; do sudo mount --bind "/$d" "$ROOT/$d"; done
# FIX: always unmount, even if a step fails (set -e would otherwise leave
# bind mounts behind and make the rootfs impossible to delete).
cleanup() { for d in dev sys proc; do sudo umount -l "$ROOT/$d" 2>/dev/null || true; done; }
trap cleanup EXIT

sudo chroot "$ROOT" apt-get update
# FIX: package names. "fltk1.3" is a source package (binary: libfltk1.3);
# ca-certificates is required now that LiteBird verifies TLS certificates;
# a plain linux-image-generic kernel is used instead of the lts-xenial meta.
sudo chroot "$ROOT" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libfltk1.3 libcurl4 ca-certificates \
    xserver-xorg-core xinit openbox \
    xterm busybox-static \
    linux-image-generic  # or another suitable 32-bit kernel

echo "=== 3. Install LiteBird ==="
# Cross-compile or build inside chroot
sudo install -m 755 litebird "$ROOT/usr/local/bin/litebird"

echo "=== 4. Autostart on boot ==="
sudo mkdir -p "$ROOT/etc/skel/.config/openbox"
cat << 'EOF2' | sudo tee "$ROOT/etc/skel/.config/openbox/autostart" >/dev/null
litebird &
EOF2

# Simple xinitrc
cat << 'EOF2' | sudo tee "$ROOT/root/.xinitrc" >/dev/null
openbox &
exec litebird
EOF2

echo "=== 5. Package ISO (example using live-boot or manual) ==="
# For a true live ISO use the 'live-build' package:
#   lb config --architecture i386 --distribution bionic --binary-images iso-hybrid
#   cp -a $ROOT/* config/includes.chroot/
#   lb build
#
# Or create a simple bootable image with a kernel + initrd + squashfs.
# For v86 testing, the easiest path is often:
#   - Use an existing TinyCore or Alpine ISO
#   - Boot it
#   - Install the prebuilt 32-bit litebird binary + libs

echo "Done. See README.md for more practical v86 instructions."
echo "Recommended for v86: prebuild a static or lightly dynamic binary and drop it into Tiny Core Linux or a custom Buildroot image."
