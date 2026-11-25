#!/bin/bash
# build-iso-outline.sh
# Outline for packaging LiteBird into a minimal bootable ISO usable with copy.sh/v86
# Run this on a host that can do i386 debootstrap (install qemu-user-static if needed).
# This is a high-level guide; adjust paths and packages as needed.

set -e
ROOT=litebird-root
ISO=litebird-ubuntu-i386.iso

echo "=== 1. Create minimal i386 Ubuntu rootfs (Bionic 18.04) ==="
sudo debootstrap --arch=i386 --variant=minbase bionic "$ROOT" http://archive.ubuntu.com/ubuntu

echo "=== 2. Prepare chroot ==="
sudo cp /usr/bin/qemu-i386-static "$ROOT/usr/bin/" || true
sudo chroot "$ROOT" apt-get update
sudo chroot "$ROOT" apt-get install -y \
    fltk1.3 libcurl4 \
    xserver-xorg-core xinit openbox \
    xterm busybox-static \
    linux-image-generic-lts-xenial  # or suitable 32-bit kernel

echo "=== 3. Install LiteBird ==="
# Cross-compile or build inside chroot
sudo cp litebird "$ROOT/usr/local/bin/"
sudo chmod +x "$ROOT/usr/local/bin/litebird"

echo "=== 4. Autostart on boot ==="
sudo mkdir -p "$ROOT/etc/skel/.config/openbox"
cat << 'EOF' | sudo tee "$ROOT/etc/skel/.config/openbox/autostart"
litebird &
EOF

# Simple xinitrc
cat << 'EOF' | sudo tee "$ROOT/root/.xinitrc"
openbox &
litebird
EOF

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
