# Why the full Ladybird engine is not included (and what we did instead)

Ladybird’s rendering engine (**LibWeb**) + JavaScript engine (**LibJS**) is a full, independent, standards-oriented browser engine written from scratch in C++ (with growing Rust components). It is **not** a lightweight embeddable library suitable for a minimal bootable ISO under copy.sh/v86.

## Practical constraints

| Aspect | Reality for Ladybird / LibWeb | Fit for minimal v86 Ubuntu ISO |
|--------|-------------------------------|--------------------------------|
| Build requirements | C++23, CMake 3.30+, Ninja, Rust toolchain, Qt6 or GTK, 8+ GB RAM, 30+ GB disk | Minimal live systems have far less |
| Binary / runtime size | WebContent process commonly hundreds of MB per page (even after recent optimizations) | v86 typically has limited RAM (often 128–512 MB total) |
| Dependencies | LibCore, LibGfx, LibIPC, LibHTTP, LibTLS, LibUnicode, many others + system libs | Pulls in a large graph that defeats the “minimal” goal |
| Extractability | No official standalone “lightweight LibWeb” or embeddable subset | Full engine or nothing; partial extraction is a multi-month project |
| Multi-process | Core design (Hub, WebContent, RequestServer, ImageDecoder, Compositor) | Adds complexity and resource use unsuitable for constrained environments |

There is an experimental GTK wrapper project (`libweb-gtk`) that tries to embed LibWeb, but it is a proof-of-concept, still requires building the full engine, and is not production-ready or lightweight.

## What LiteBird does instead

LiteBird takes the **best architectural ideas** from Ladybird and implements a deliberately minimal equivalent that *can* run on a 32-bit Ubuntu-style live image under v86:

- **Separation of concerns** — `NetworkFetcher` (inspired by RequestServer) is completely isolated from the UI and rendering path.
- **Event-driven design** — FLTK event loop + callbacks (echoing LibCore’s EventLoop).
- **Controlled loading of untrusted content** — Link interception, error pages, timeouts, no blind execution of arbitrary scripts.
- **Modular structure** — Clear boundaries so a more capable renderer could later be swapped in.
- **Tiny footprint** — Only FLTK + libcurl + standard C++. The binary is small enough for minimal live systems.

The rendering backend is currently FLTK’s `Fl_Help_View` (HTML 2.0-level support, basic tables, images, and links). This is the practical “lightweight engine” for the stated constraints.

## If you still want the real Ladybird engine

1. Build full Ladybird on a capable host following the official instructions:  
   https://github.com/LadybirdBrowser/ladybird/blob/master/Documentation/BuildInstructionsLadybird.md
2. Run it natively (it is not designed for v86).
3. For embedding experiments, look at the `libweb-gtk` proof-of-concept, but expect a large, resource-heavy result.

LiteBird remains the component that satisfies the original request: a C++ + FLTK browser that can be compiled and run on a minimal bootable Ubuntu-derived ISO compatible with copy.sh/v86, while still drawing on Ladybird’s design principles.
