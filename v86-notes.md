# Additional notes for 32-bit / v86 builds

## Cross-compiling a 32-bit binary (on a 64-bit Debian/Ubuntu host)

```bash
sudo apt-get install gcc-multilib g++-multilib libfltk1.3-dev:i386 libcurl4-openssl-dev:i386
# or use a chroot / qemu-user

# Simple multiarch attempt (may need more setup)
g++ -m32 -std=c++17 -O2 -o litebird32 litebird.cpp \
    `fltk-config --cxxflags --ldflags` -lcurl
```

If multiarch is painful, build inside an i386 chroot or use the live ISO itself to compile.

## Testing the binary under v86

1. Boot a known-good GUI ISO (Tiny Core, Xubuntu 18.04 i386, etc.) in https://copy.sh/v86/
2. Transfer the `litebird` binary (and any missing .so files) into the guest.
3. Run `ldd litebird` inside the guest to see missing libraries.
4. Install the matching packages or copy the .so files.

## Minimal runtime dependencies

- libfltk.so.1.3
- libcurl.so.4
- libX11, libXext, libXft (pulled in by FLTK)
- A running X server (openbox, fluxbox, or even just `xinit`)

For the absolute smallest image, statically link as much as possible or use a Buildroot config that includes FLTK + curl + X.
