# LiteBird

A lightweight browser written entirely in C++ using FLTK, inspired by the best architectural ideas from the Ladybird browser engine.

**See [ENGINE.md](ENGINE.md) for a detailed explanation of why the full Ladybird LibWeb engine cannot be included in a minimal v86/Ubuntu ISO build, and what we implemented instead.**

## Inspiration from Ladybird

Ladybird (https://ladybird.org) is an independent browser engine written primarily in C++. Key ideas adopted here in simplified form for a minimal footprint:

- **Separation of concerns**: Networking (`NetworkFetcher`, modeled after RequestServer) is isolated from UI/rendering.
- **Event-driven design**: FLTK event loop + callbacks, echoing LibCore’s EventLoop.
- **Controlled content loading**: Link interception and error pages instead of blindly executing untrusted content.
- **Modular engine boundary**: A thin `BrowserEngine` abstraction sits between the network layer and the FLTK renderer so a stronger backend could be swapped later.
- **Lightweight rendering**: Currently uses FLTK’s `Fl_Help_View` (basic HTML 2.0 + tables + images). Suitable for documentation, simple pages, and low-resource environments.

This is **not** a full modern browser. It is intentionally minimal so it can boot and run on constrained systems such as a 32-bit Ubuntu live ISO under the copy.sh/v86 emulator.

## Features

- Address bar with Enter support
- Go / Back / Forward buttons
- History stack
- Fetches pages via libcurl (HTTP/HTTPS)
- Displays basic HTML in Fl_Help_View
- Intercepts http(s) links for in-browser navigation
- Status bar
- Error pages for network/HTTP failures

## Building

```bash
# On Debian/Ubuntu
sudo apt-get update
sudo apt-get install -y build-essential libfltk1.3-dev libcurl4-openssl-dev

cd litebird
make
./litebird
```

Static linking is possible but increases size; for the ISO we prefer dynamic and install the shared libraries.

## Running on a Minimal Bootable Ubuntu ISO for copy.sh/v86

v86 (https://copy.sh/v86/) emulates a 32-bit x86 PC in the browser. It works well with older Ubuntu i386 images (16.04 / 18.04 community flavors) or custom minimal systems.

### Recommended approach (practical)

1. Download an existing lightweight 32-bit live ISO that already has X11:
   - Xubuntu 18.04 i386
   - Lubuntu 18.04 i386
   - Or use Tiny Core Linux (much smaller) and add the binary.

2. Boot it in v86 (or QEMU for testing).

3. Inside the live session install the build tools and compile LiteBird, **or** cross-compile the binary on a 32-bit chroot and copy it in.

### Creating a custom minimal Ubuntu ISO (outline)

Use `debootstrap` + `live-build` or a simple chroot + `grub-mkrescue` / `xorriso`.

Example high-level steps (run on a 64-bit host with qemu-user-static for i386):

```bash
# Create i386 rootfs
sudo debootstrap --arch=i386 bionic litebird-root http://archive.ubuntu.com/ubuntu

# Enter chroot (with qemu-i386-static)
sudo cp /usr/bin/qemu-i386-static litebird-root/usr/bin/
sudo chroot litebird-root

# Inside chroot
apt-get update
apt-get install -y fltk1.3 libcurl4 xorg openbox xterm
# Copy litebird binary and create desktop autostart
# Configure openbox to start litebird
# exit

# Package into ISO with live-boot or simple initramfs + kernel
```

For the absolute smallest size that still runs a GUI browser, prefer **Buildroot** or **Alpine Linux** images that v86 already supports well, then install or statically link LiteBird.

## Limitations

- Fl_Help_View supports only a subset of HTML (no modern CSS, limited JS, no complex layouts).
- No JavaScript engine (Ladybird's LibJS is far larger).
- SSL verification disabled by default for minimal systems (enable for production).
- Single process (Ladybird uses multi-process isolation; that would require significant extra code and IPC).

## License

MIT. Use freely. Inspired by, but not derived from, Ladybird source.

## Next Steps

- Add a simple CSS subset renderer
- Optional embedded tiny JS interpreter
- Static binary build for even smaller ISO
- Pre-built 32-bit binary and ready-to-boot ISO script
