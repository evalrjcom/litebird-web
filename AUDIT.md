# LiteBird code audit

Audited: `evalrjcom/litebird-web` at `main` = `489833d` (PR #1 merged). Every file was reviewed: `litebird.cpp`, `CMakeLists.txt`, `Makefile`, `.github/workflows/build.yml`, `build-iso-outline.sh`, `README.md`, `ENGINE.md` and `v86-notes.md`. The repo has no headers.

Every change in the corrected files is marked inline with `// FIX:` (a bug, a security issue or an edge case) or `// CHANGED:` (behaviour or efficiency). Markdown and shell files use `# FIX` or `<!-- CHANGED -->`.

References are written as `original line(s) → corrected line(s)`.

## How it was verified (Linux, Debian 13, FLTK 1.3.11, libcurl 8.14.1)

- **Builds:** `make` and `cmake -S . -B build && cmake --build build` both build cleanly with **0 warnings** under `-Wall -Wextra`.
- **Unit tests:** `make test` runs `tests/test_helpers.cpp`, and all of them pass. The tests cover:
  - all 27 RFC 3986 §5.4 URL-resolution examples
  - normalising what the user types in the address bar
  - link rewriting, `<base href>`, entity decoding, and stripping comments and scripts
  - charset conversion between CP1252 and UTF-8, and handling of NUL bytes
- **GUI smoke tests** ran under Xvfb with `xdotool`, in plain, ASan+UBSan and TSan builds. Results:
  - Clicking relative links works.
  - Back, Fwd, Back works.
  - `file://` is blocked.
  - `https://example.com` loads with TLS verification on.
  - `self-signed.badssl.com` is rejected.
  - A Latin-1 page renders correctly.
  - `text/plain` is shown as text, and binary content is refused.
  - Cancelling a load to an unreachable host works.
  - Esc no longer quits.
  - The window closes cleanly with exit code 0.
  - TSan reported no races, and ASan reported no errors from LiteBird code. The only ASan leak reports came from fontconfig.
- **Original bugs reproduced:** the original binary **segfaulted** (stack overflow) on a page with `<img src="http://…">`. Before crashing it sent hundreds of `GET /img.png` requests in a loop. Its relative links also did nothing when clicked.
- **Not verified here:**
  - **The Windows/MSVC build.** It needs the GitHub Windows runner. These changes are the most likely to need a CI round-trip:
    - the `WIN32` subsystem flag
    - `/W4`
    - the `fltk::fltk` target fallback
  - **`build-iso-outline.sh`.** It was only syntax-checked (`bash -n`), not run, because it needs debootstrap and sudo.
  - **The old-libcurl `#if` branch for curl versions below 7.85.** It was not compiled here.

## (a) Changelog

### Critical (2)
- **[Critical] TLS verification disabled.** `litebird.cpp:48-49 → 577-582`. `CURLOPT_SSL_VERIFYPEER/VERIFYHOST = 0` let anyone on the network impersonate any HTTPS site. Verification is now on, and there is an explicit `--insecure` flag (`:990-996`). Certificate failures now show an actionable error (`:599-608`).
- **[Critical] Crash or infinite navigation from `<img>`.** `litebird.cpp:202-210 → 938-975`.
  - Fl_Help_View calls the link callback for every image on every layout and redraw. The old callback *navigated* to absolute image URLs. That reloaded the page inside its own layout, recursed until the stack overflowed, and segfaulted (reproduced).
  - Returning `nullptr` for images also crashes, because FLTK dereferences it without a check.
  - Image requests now get `""`, which shows FLTK's broken-image icon and causes no network or disk access.

### High (5)
- **[High] Links opened in the system browser.** `litebird.cpp:202-210 → 459-517, 938-975`.
  - Fl_Help_View's `load()` sends every absolute `http(s)` link to `fl_open_uri()` (xdg-open) *before* it asks the link callback. Clicking a normal link therefore left LiteBird.
  - All `<a href>` values are now rewritten to an unguessable per-session token (`lb<random>:N`). The real, fully resolved URLs are kept in a side table, and navigation is deferred to the event loop (`:969-974`).
- **[High] The UI froze during every page load.** `litebird.cpp:116-126 → 782-829, 866-877`.
  - curl ran on the UI thread, so the window couldn't redraw or close for up to 25 s.
  - Loads now run on a worker thread and post results with `Fl::lock()`/`Fl::awake()` (`:999`, `:817-822`).
  - A newer navigation cancels the older one through the progress callback (`:583-587`, `:630-633`), and stale results are dropped with a generation counter (`:874`).
- **[High] Back/Forward history was corrupt.** `litebird.cpp:183-200 → 885-901, 913-928`.
  - Back re-pushed the page and then popped it, which wiped the forward history.
  - Fwd popped the entry it was about to visit.
  - In practice Fwd never worked, and Back could skip pages. History is now a proper index with Push and Traverse modes, duplicate reloads are skipped, and the Back/Fwd buttons grey out when unusable (`:853-856`).
- **[High] Relative links were resolved against the temp file.** `litebird.cpp:128-161 → 293-332, 459-517`.
  - Pages were loaded from `/tmp/lbXXXX.html`, so `about.html` became `/tmp/about.html`. That link was broken, and the click read local files instead.
  - Links are now resolved per RFC 3986 against the page's *post-redirect* URL, and `<base href>` is honoured.
- **[High] `build-iso-outline.sh` could not succeed.** `:13-14 → 26-45`. The package `fltk1.3` doesn't exist (the binary package is `libfltk1.3`). `openbox` and `libfltk1.3` are in *universe*, which debootstrap doesn't enable. Both are fixed.

### Medium (11)
- **[Medium] No scheme allowlist.** `litebird.cpp:39, 118 → 352-385, 568-575`. `file://`, `gopher://`, `dict://` and other schemes went straight to curl. Requests and redirects are now limited to http and https.
- **[Medium] Remote pages could make FLTK read local files.** `litebird.cpp:202-210 → 938-958`. Image paths like `<img src="/etc/…">` were read as local images, and non-http links loaded local files. Both are now blocked.
- **[Medium] Unbounded download size.** `litebird.cpp:40-43 → 523, 617-627`. A huge or endless response could exhaust memory. Downloads are now capped at 8 MB.
- **[Medium] Exceptions escaped through curl's C callback.** `litebird.cpp:40-43 → 617-627`. `std::bad_alloc` thrown inside libcurl's C callback is undefined behaviour. The callback now catches it and aborts the transfer cleanly.
- **[Medium] Scheme detection used `find("://")`.** `litebird.cpp:118-119 → 334-385`.
  - `example.com/?u=http://x` was sent without a scheme.
  - Spaces and non-ASCII characters made curl fail.
  - Empty input became `https://`.
  - Input is now trimmed, percent-encoded, validated and normalised (host lower-cased, path `/`).
- **[Medium] The final URL after redirects was ignored.** `litebird.cpp:51-54 → 590-594, 899`. The address bar, the history and relative-link resolution all used the pre-redirect URL.
- **[Medium] Charset mojibake.** `litebird.cpp:141,144,159 → 139-176, 683-687`. ISO-8859-1 and Windows-1252 pages showed garbage characters. They are now converted to UTF-8, with invalid bytes repaired.
- **[Medium] Binary content was dumped into the view.** `litebird.cpp:60-62 → 677-681`. Images, PDFs and zips were "rendered" as HTML, and NUL bytes truncated pages. Non-text types now show an error page, and NUL bytes are stripped (`:148`).
- **[Medium] Esc closed the browser.** FLTK's default window callback hides the window on Escape, so pressing Esc in the address bar quit LiteBird. Now only the window manager closes it (`:760-766`).
- **[Medium] Temp-file handling.** `litebird.cpp:128-161 → removed (34-36)`.
  - The return value of `rename()` was unchecked, and the `mkstemp` file leaked if rename failed.
  - `ofstream` errors were unchecked, and `/tmp` was hard-coded instead of using `$TMPDIR`.
  - `GetTempPathA` mangled non-ASCII Windows user names, because FLTK expects UTF-8 paths.
  - The page is now given to Fl_Help_View in memory, so there are no temp files at all.
- **[Medium] Missing `ca-certificates` and chroot mounts.** `build-iso-outline.sh:12-17 → 29-45`.
  - `ca-certificates` was missing from the ISO, and HTTPS needs it now that TLS is verified.
  - apt ran in the chroot without `/proc`, `/sys` and `/dev` mounted. They are now bind-mounted, with a `trap` that unmounts them.

### Low (19)
- `litebird.cpp:214 → 985-989`: `curl_global_init` failure is now checked.
- `:221 → 1000-1007`: `curl_global_cleanup()` used to run while the window was still alive. The window and its threads are now torn down first.
- `:216-219 → 990-997`: added argument parsing with `--insecure` and `--help`. Previously `argv[1]` was always treated as a URL.
- `:35-54 → 547-551`: the curl handle is now held by RAII.
- `:39 → 554-558`: the `CURLOPT_URL` result is checked.
- `:45 → 562`: `MAXREDIRS 10` is set. libcurl before 8.3 followed redirects without limit.
- `→ 565`: `CURLOPT_NOSIGNAL` is set, which libcurl needs for multi-threaded use.
- `:66-70 → 58-73, 636-643`: the error page now HTML-escapes its message and shows the failing URL.
- `:121,169 → 76-85, 852`: `@` in URLs, such as `medium.com/@user`, was parsed as an FLTK label symbol in the status bar. It is now escaped.
- `:205` (fixed in `→ 44-47`): links written as `HTTP://` (upper case) weren't intercepted.
- `→ 661, 882`: `#fragment` is no longer sent to the server, and the page now scrolls to the fragment.
- `→ 466-516`: `<script>` and `<style>` contents were rendered as visible text. They are now removed.
- **Long hrefs were silently cut.** Fl_Help_Link truncates hrefs at 191 characters. Tokens avoid this.
- `:88-99 → 730-745`: the toolbar buttons stretched and drifted on window resize. They now sit in a group with the URL field as the resizable widget, and `size_range` stops the window collapsing (`:759`).
- `→ 902-906`: the window title now shows the page `<title>`.
- **Makefile:**
  - `-pthread` added.
  - `$(shell fltk-config)` with a clear error if it's missing.
  - FLTK flags moved to the compile step.
  - `PREFIX`/`DESTDIR` honoured in `install`.
  - New `test` target.
- **CMakeLists.txt:**
  - Dropped the unused `fltk_forms`, `fltk_images` and `fltk_gl`. `fltk_gl` pulled in OPENGL32.
  - `fltk::fltk`/`fltk` target fallback.
  - `Threads::Threads` added.
  - `WIN32` GUI subsystem, so no console window opens.
  - Warnings turned on, Release set as the default build type, and an `install()` rule added.
- **build.yml:**
  - `permissions: contents: read`.
  - `concurrency` cancels superseded runs.
  - `timeout-minutes: 45`.
  - New `make test` step.
- **Script and docs:**
  - `build-iso-outline.sh`: `set -euo pipefail`, an upfront check for the i386 binary, `exec litebird` in `.xinitrc`, and quiet `tee`.
  - README: the `cd litebird` path and the package names are fixed.
  - v86-notes: `-pthread` added.

**Totals: 37 fixes: 2 Critical, 5 High, 11 Medium, 19 Low.**

## (b) Efficiency and effectiveness suggestions
1. **Cache pages in memory for Back/Fwd.** Keep the last N `Page` objects (HTML plus link table) keyed by URL. History traversal becomes instant and costs no network round trip on slow v86 links.
2. **Reuse connections.** Keep one long-lived `CURL*` per worker, or a `CURLSH` share for DNS, TLS sessions and connections. That saves a TCP and TLS handshake on every click to the same host, which is the dominant cost under emulation.
3. **Add vcpkg binary caching and a manifest to CI.** Add `vcpkg.json` with a pinned `builtin-baseline` and cache the vcpkg archives (`VCPKG_DEFAULT_BINARY_CACHE` plus `actions/cache`). This would cut the roughly 10-minute Windows job to about 2 minutes and make builds reproducible.
4. **Add a sanitizer and test job to CI.** An ASan+UBSan build runs `make test`, then a short Xvfb + xdotool smoke test, the same one used in this audit. This catches crashes like the `<img>` one before they ship.
5. **Split `litebird.cpp` into modules.** For example `url.cpp`, `html_prep.cpp`, `net.cpp` and `ui.cpp`, with headers, and register the tests with `ctest`. The pure helpers are already isolated, so the split is mechanical.
6. **Add a Stop button and a progress readout.** The existing progress callback can report bytes received to the status bar through `Fl::awake`. Stop just sets the cancel flag.
7. **Add keyboard shortcuts.** Ctrl+L (focus the URL bar), Alt+←/→ (back and forward), F5/Ctrl+R (reload) and Ctrl+F. These are cheap and make a big usability difference in a VM window.
8. **Fall back to http.** When a bare host was typed and the guessed `https://` fails to connect, retry with `http://`. Many intranet and retro sites are HTTP-only.
9. **Pin GitHub Actions to commit SHAs and add a tag-triggered release job.** The release job would publish binaries plus SHA-256 checksums, so users don't have to dig into run artifacts.
10. **Ship a Linux build that runs anywhere.** Either a static or musl i386 build, or an AppImage. That matches the v86/Tiny Core goal better than installing shared libraries.

## (c) Innovative feature ideas
1. **Search from the address bar.** Input that isn't a URL goes to DuckDuckGo Lite or another text-only search engine. Those pages render well in Fl_Help_View, so LiteBird becomes usable without knowing URLs.
2. **Opt-in image loading with budgets.** Fetch `<img>` through NetworkFetcher into a capped in-memory cache and give Fl_Shared_Image a local path. Images then show up without losing the current safety guarantees.
3. **A Ladybird-style RequestServer process.** Fork the network and HTML-prep code into a sandboxed helper process (pipe IPC, seccomp or Landlock on Linux). This is real crash and exploit isolation, true to the project's Ladybird inspiration.
4. **Reader mode.** Extract the main article (text-density heuristic) and drop navigation and boilerplate. This plays to Fl_Help_View's simple layout and makes modern sites readable.
5. **Gemini and Markdown rendering.** Lightweight protocols and formats map perfectly onto HTML 2.0 and are ideal for low-bandwidth emulators.
6. **A built-in tracker and ad blocklist.** Every request already goes through one function, so a hosts-style blocklist is about 30 lines and saves bandwidth.
7. **Find in page.** `Fl_Help_View::find()` already exists. Wire it to Ctrl+F with a small search bar.
8. **Persistent bookmarks and history** in `~/.config/litebird/`, plus a `litebird://history` internal page.
9. **Basic GET forms.** Parse `<form method=get>` into native FLTK inputs, which is enough for search boxes and wikis.
10. **Tabs.** An `Fl_Tabs` container with a per-tab history and worker, reusing the new `BrowserUI` navigation core.

---

## Follow-up after Windows testing of PR #2

Testing the PR #2 build on Windows turned up three problems:
1. On `https://lite.duckduckgo.com/lite`, only the heading rendered. Fl_Help_View drops `<form>` and `<input>`, so the search box and button never appeared.
2. Clicking a DuckDuckGo result (`https://duckduckgo.com/l/?uddg=…&rut=…`) showed a blank page.
3. The user asked for dark mode.

### Changes (marked `// FEATURE:` / `// FIX:` / `// CHANGED:` in `litebird.cpp`)
- **[High] Web forms.**
  - **Parsing.** `prepare_html()` (`:619`) turns every `<form>`, `<input>`, `<textarea>`, `<select>`/`<option>` and `<button>` into plain data (`Form`/`FormField`). Controls outside any `<form>` go into one implicit GET form.
  - **In the page.** Each control is replaced by an inline placeholder link, such as `[q: ____]` or `[ Search ]`. Clicking a field placeholder focuses its widget, and clicking a submit placeholder submits the form.
  - **Form bar.** `build_panel()` (`:1582`) builds real FLTK widgets in a scrollable bar under the toolbar, at most 3 rows with a scrollbar beyond that:
    - `Fl_Input` and `Fl_Secret_Input`
    - `Fl_Multiline_Input`
    - `Fl_Choice`
    - `Fl_Check_Button` and `Fl_Round_Button` (radio exclusivity is per form and name)
    - `Fl_Button`
  - **Submitting.** `submit_form()` (`:1701`) collects the values the way the HTML spec does: the submitter's name and value, checked boxes only, and the selected option value. Enter in a text field submits through the form's first submit button.
  - **Encoding.** GET replaces the action URL's query with `form_urlencode()` (`:558`). POST sends `application/x-www-form-urlencoded` through `CURLOPT_COPYPOSTFIELDS`.
  - **Why this design.** Fl_Help_View can't embed widgets or report where elements are laid out, so overlaying widgets on the page isn't possible. A form bar plus inline placeholders always works, keeps the page readable, and still shows where each control sits.
- **[High] DuckDuckGo result links.**
  - `unwrap_redirector()` (`:831`) decodes `uddg` from `duckduckgo.com/l/` and its subdomains, and LiteBird opens the target directly.
  - It is only applied to DDG hosts and only for http(s) targets, so `javascript:` and look-alike hosts are refused.
  - The interstitial page DDG serves contains a `<meta refresh>` plus `window.location.replace(...)`. It now also works through the generic redirect handling below.
- **[Medium] `<meta http-equiv="refresh">`.** `parse_meta_refresh()` (`:845`) follows a refresh with a delay of 5 s or less. A plain "reload" refresh is ignored.
- **[Medium] Simple JavaScript redirects.**
  - `find_js_redirect()` (`:884`) recognises `location[.href] = "…"` and `location.replace/assign("…")` inside `<script>` blocks, and undoes `\/` and `\u0026` escapes.
  - It only acts on near-empty pages (fewer than 200 visible characters), so ordinary pages with scripts are left alone.
  - `BrowserEngine::load()` (`:1111`) follows at most 5 of these soft redirects and stops on self-refresh. Only http(s) targets are followed.
- **[Medium] Dark mode.**
  - A toolbar button and **Ctrl+D** toggle it. Shortcuts are caught with `Fl::event_dispatch` (`:1783`), so `Fl_Input`'s own Ctrl+D binding doesn't swallow the key.
  - `apply_theme_colors()` sets `Fl::background`, `background2`, `foreground` and the selection colour, plus the help view's background and text colours.
  - `apply_dark_theme()` (`:931`) injects `<body bgcolor text link>` (links are `#8ab4f8`) and strips `bgcolor` and `<font color>` so pages stay readable.
  - The choice is saved by `save_dark_setting()`, read back by `load_dark_setting()` (`:1214`):
    - Windows: `%APPDATA%\LiteBird\config.ini`, using wide-char APIs.
    - Linux: `$XDG_CONFIG_HOME/litebird/config`, defaulting to `~/.config/litebird/config`.
  - `--dark` and `--light` override the saved choice for one session.
- **[Medium] History page cache.** `traverse()` (`:1520`):
  - Back and Forward reuse the cached page, so they are instant and a POST search is never re-submitted.
  - The cache covers the 10 entries on each side of the current one.
  - A POST result is always a new history entry, even when its URL is unchanged (DDG Lite posts back to `/lite/`).
- **[Low] Smaller changes:**
  - Keyboard shortcuts: Ctrl+L, F5 or Ctrl+R, Alt+Left/Right.
  - `CURLOPT_AUTOREFERER` is on.
  - The User-Agent is now `LiteBird/1.2`.
  - The widget scheme is `gtk+`.
  - A `.gitignore` was added.

### Tests
- `tests/test_helpers.cpp` adds checks for:
  - uddg decoding, including rejecting look-alike hosts, wrong paths and `javascript:`
  - meta-refresh parsing: quoted URLs, entities, plain self-refresh, and `name=` versus `http-equiv=`
  - JS redirect patterns, including that a comparison is not treated as a redirect
  - form URL-encoding, including UTF-8
  - extraction of a DDG Lite-like form with every control type
  - the dark-theme rewrite
- **All tests pass.**
- **GUI tests (Xvfb + xdotool, in ASan+UBSan and TSan builds, no errors).** DuckDuckGo is unreachable from the build box (its TLS handshake is refused there), so I ran them against a local mock that serves DDG Lite's markup and DDG's `/l/` interstitial page. Results:
  - Typing `python` and pressing Enter sent `POST /lite/` with body `q=python&kl=&df=`.
  - The interstitial result link followed the meta refresh to the target.
  - A `https://duckduckgo.com/l/?uddg=` link went straight to the target without contacting DDG.
  - A JS-only redirect page was followed.
  - Back returned to the results page from the cache, with no re-POST.
  - Ctrl+D switched to dark mode, the setting was written to the config file, and it was still in effect after a restart.
- **Screenshots** are in `/workspace/litebird-audit/screens/`.

### Limitations
- **No JavaScript engine.** Only the redirect patterns above are recognised, so script-driven forms and buttons won't work.
- **No CSS, no images and no file uploads.** `<input type=file>`, `button` and `reset` are skipped.
- **Approximate form placement.** Controls appear in the form bar, not at their exact position in the page; the inline placeholders mark where they are.
- **Redirect unwrapping is DuckDuckGo-only.** Other search engines' redirect links still go through their own redirect pages; meta refresh and the JS patterns usually handle them.
