# LiteBird — second audit (AUDIT-2)

**Audited:** PR #2, branch `audit/fixes-2026-11` at `0e5493d`. The PR is untouched: nothing was pushed or changed. The corrected tree is in this folder.

Every change in `litebird.cpp` is marked `// FIX(2):` (a bug, security issue or edge case) or `// CHANGED(2):` (behaviour or efficiency). There are 98 markers. Line numbers below refer to the corrected `litebird.cpp` (2,636 lines).

**Totals:** 36 findings: **3 High, 14 Medium, 19 Low**.

"Repro" means the bug was reproduced against the PR head (`0e5493d`) with the GUI smoke test and the mock server. "Reasoned" means it was found by reading the code (and covered by unit tests where possible).

## Changelog

### High
1. **[High] The whole browser crashed on a page with a long JS redirect string.** `std::regex` in `find_js_redirect` recurses once per character, so a `location.href='…'` literal of about 50 KB overflows an 8 MB thread stack. Windows threads have 1 MB stacks, so there it takes only about 6 KB.
   - **Repro:** the PR build gets SIGSEGV (exit 139) on `/bigjs` (120 KB literal), and ASan reports `stack-overflow` on the worker thread.
   - **Fix:** a hand-written scanner, `scan_js_location()` `:1215`, with no recursion and an 8 KB literal cap. `<regex>` is removed (`:45`).
2. **[High] A `<select>` sent the wrong value, or no value at all.**
   - **Cause:** `Fl_Menu_::add()` merges items that have the same label (`--`, `Any`, `Other`…), so the menu has fewer items than the `<select>` has options.
   - **Repro:** for options `A, --, --, B(selected)` the PR sent `?q=x` with no `k` at all. The fixed build sends `k=b`.
   - **Fix:** `make_choice()` `:2206` builds a `Fl_Menu_Item` array and `copy()`s it, so there is one item per option. Labels are kept for the widget's lifetime, over two generations (`:2008`).
3. **[High] Keystrokes were hijacked when a load finished.** When a page arrived, its first text field took focus and the URL bar was overwritten, even while the user was typing a new address.
   - **Repro:** the PR sent the rest of the typed text into the page's form and submitted it to that site (`GET /echo?z=XY`), and the typed address was lost.
   - **Fix:** `user_typing_url()` `:2041`. The URL bar is not overwritten while it is being edited (`:2110`), and focus is not stolen (`:2327`). An `autofocus` field wins over "first field".

### Medium
4. **[Medium] `enctype` was ignored.** `multipart/form-data` forms got an urlencoded body the server can't parse, and `text/plain` was ignored too.
   - **Repro:** the PR sent `application/x-www-form-urlencoded 'q=hello&t=l1%0Al2'`.
   - **Fix:** `form_multipart()` / `form_text_plain()` (`:725`), with a random boundary and a Content-Type header through `CURLOPT_HTTPHEADER` (`:2441`, `:1395`). Field names are escaped as the spec requires (`"`, CR, LF).
5. **[Medium] Forms on Windows-1252/Latin-1 pages were submitted as UTF-8,** so `café €` arrived as `caf%C3%A9+%E2%82%AC` (repro).
   - **Fix:** `utf8_to_cp1252()` `:345` with `&#NNNN;` for unmappable characters, plus `accept-charset` support (`:948`, `:2437`). It now sends `caf%E9+%80`.
6. **[Medium] False JS redirects.** The regex had no word boundary, so `var geolocation = 'Paris'` on a short page navigated to `/Paris` (repro: `GET /Paris 404`). Comparisons such as `location.href == 'x'` also counted as assignments.
   - **Fix:** the scanner (`:1215`) checks identifier boundaries, rejects `==`, and decodes `\'`, `\"`, `\/`, `\xNN` and `\uNNNN`.
7. **[Medium] Meta-refresh problems:**
   - Refresh tags inside comments or `<script>` strings triggered redirects.
   - `content="-1"` or `content="abc"` was treated as a URL.
   - `atoi()` overflowed on huge delays (undefined behaviour that could turn into an instant redirect).
   - **Fix:** the parse now runs on the prepared HTML (`:1590`), and the delay is validated and clamped (`:1182`).
8. **[Medium] uddg and soft-redirect targets weren't normalised.** A decoded `%00` became a NUL, so curl loaded a truncated URL that differed from the one shown. Spaces and control bytes made curl fail.
   - **Fix:** targets go through `normalize_user_url()` (`:1133`, `:1605`).
   - Nested DDG `/l/` links (ad links) are unwrapped up to 4 levels by `unwrap_all()` `:1154`.
9. **[Medium] Redirect loops ended silently.** A loop (A→B→A) or hitting the 5-hop limit left the user on a blank interstitial (repro: the PR showed `/loop2` blank).
   - **Fix:** a seen-set plus an explanatory error page (`:1517`).
10. **[Medium] An unclosed `<select>` or `<button>` swallowed the rest of the page.** With many of them, each one rescanned to the end of the page (quadratic).
    - **Fix:** `find_first_tag()` bounds them by the tags that implicitly close them (`:1023`, `:1060`). 20,000 unclosed selects now parse in about 0.03 s with the page text kept.
11. **[Medium] Nested `<form>`.** The inner start tag opened a new form, so its controls were submitted to the wrong action and method. The HTML spec ignores it.
    - **Fix:** `:940`.
12. **[Medium] Unbounded memory.** The ±10-entry page cache could hold about 21 × 8 MB pages plus their tables, and history grew forever.
    - **Fix:**
      - a 48 MB byte budget, applied nearest page first (`Page::bytes()`, `trim_cache()` `:2123`, `:1787`)
      - a 500-entry history cap (`:2100`)
13. **[Medium] Stale deferred submit.** A load finishing between a click on `[ Search ]` and the deferred submit applied the old indices to the *new* page's forms.
    - **Fix:** `pending_page_` weak_ptr (`:2008`, `:2527`).
14. **[Medium] An open dropdown vanished when a load finished.** The page swap deleted the open `Fl_Choice` from inside the popup's nested event loop. The repro shows the popup disappearing and the choice being lost. FLTK 1.3.11's widget tracker happened to prevent a crash; this is a latent use-after-free.
    - **Fix:** `on_loaded` waits while `Fl::grab()` is set (`:2070`). Before/after screenshots were taken (not committed).
15. **[Medium] Config I/O:**
    - Only 2 directory levels were created, so a deep `XDG_CONFIG_HOME` never saved.
    - `fopen("wb")` truncated the file before writing, so a crash or a full disk could leave it empty.
    - Every line other than `dark=` was dropped.
    - Failures were silent.
    - **Fix:** `mkdir_p` `:1659`, an atomic temp+rename write `:1632` (Windows: `_wremove` + `_wrename`), and `update_dark_setting()` `:1723`, which keeps other lines. The parser now tolerates a BOM, CRLF and `yes/on`, and file reads are capped at 64 KB.
16. **[Medium] An invalid typed URL didn't cancel the load in flight.** That load later replaced the error page and pushed a history entry.
    - **Fix:** `++gen_` plus a cancel (`:1903`).
17. **[Medium] Form-bar widgets were deleted synchronously from inside their own callbacks.** Example: Enter in a field whose form action is invalid triggers an error page and a rebuild. ASan stayed quiet on FLTK 1.3.11, but the API doesn't guarantee that.
    - **Fix:** `clear_panel()` detaches the widgets and uses `Fl::delete_widget` (`:2234`).

### Low
18. **[Low] Entities:**
    - Named entities (`&nbsp; &copy; &eacute; &mdash; &euro;` …) are now decoded (`:211`).
    - `&#128;`–`&#159;` map through cp1252.
    - `&#0;`, surrogates and out-of-range values become U+FFFD.
    - `&# 65;` and `&#-1;` are rejected (`:247`).
19. **[Low] `decode_entities` was quadratic.** It scanned to the end of the string for `;` after every `&`. It now looks ahead at most 32 bytes and has a fast path (`:256`).
20. **[Low] UTF-8-unsafe truncation of captions and values** produced invalid UTF-8. Fixed by `utf8_truncate()` `:297`.
21. **[Low] Byte-order marks:** UTF-16 pages (FF FE / FE FF) were garbage, and the UTF-8 BOM leaked into the view. Fixed by `decode_bom()` `:312`.
22. **[Low] Dark mode deleted text after a malformed attribute.** `remove_attr` on an unclosed quote erased the tag's `>`, so the following text was swallowed. Fix at `:611`.
23. **[Low] Newlines:**
    - Textarea values are now sent as CRLF (`to_crlf` `:715`).
    - CR after `<textarea>` is handled and the CR is no longer shown (`:1005`).
    - Newlines are stripped from single-line values (`:991`).
24. **[Low] `<input type=image>`** now submits `name.x` / `name.y` (`:2425`) and uses its `alt` text as the label.
25. **[Low] GET submission dropped the action's `#fragment`.** The HTML spec keeps it (`:788`; the unit test was updated).
26. **[Low] Captions:** `<label for=id>` and wrapping `<label>` text are now used as captions (`collect_labels` `:840`).
27. **[Low] Disabled buttons:** disabled `<button>`s were clickable, and implicit submission went through a disabled default button (`:1076`, `:2390`).
28. **[Low] `size=` attribute:** `atoi(size)` overflow was undefined behaviour; the value is now clamped (`:969`).
29. **[Low] Huge forms froze the UI.** Fixes:
    - at most 300 widgets (`:2264`)
    - widget-less `<select>`s submit their default option (`:2411`)
    - giant selects are capped at 2,000 options (`:2210`)
30. **[Low] Worker robustness:**
    - `new NavResult` moved inside the `try` block
    - `catch(...)` added
    - `Fl::awake` is retried for up to 2 s instead of leaving the UI stuck in "Loading" (`:1939`, `:1966`)
    - a `std::thread` creation failure now shows an error page (`:1973`)
31. **[Low] Pending timers are removed in the destructor** (`:1873`).
32. **[Low] Every window resize reset the form bar's scroll position** to the top (`:2352`).
33. **[Low] Relative meta-refresh targets** now resolve against `<base href>` (`:1596`).
34. **[Low] `CURLOPT_POSTFIELDSIZE_LARGE`** replaces `(long)` truncation (`:1395`).
35. **[Low] The Makefile `test` target ignored `$(CPPFLAGS)`.**
36. **[Low] README said "SSL verification disabled by default".** That was stale and contradicted the code; the forms and redirects sections are updated.

### Efficiency changes made
- Each HTML page used to go through `prepare_html()` twice (once inside `visible_text_len`). It now goes through once, and meta refresh and the text count reuse the output (`:1294`, `:1590`).
- The raw response body is freed before the page is built (`:1583`), which lowers peak memory per load by about one page-size.
- `decode_entities` has a fast path and a bounded look-ahead.

## Verification (Linux, FLTK 1.3.11, libcurl 8.14.1, GCC)
- `make` and the CMake Release build both finish with **0 warnings** (`-Wall -Wextra`).
- **Unit tests** (`make test`):
  - 101 `expect()` assertions plus the RFC 3986 table and custom checks, all passing; about 60 of them are new and cover every item above that can be unit-tested
  - also pass under **ASan+UBSan** (leak detection on) and **TSan**
- **GUI smoke test** (`tests/gui/gui_smoke.sh` with `tests/gui/mock_server.py`, Xvfb + xdotool):
  - Under ASan+UBSan and under TSan, all 17 checks pass with **0 sanitizer reports**.
  - The same script against the PR head reproduces items 1, 2, 4, 5, 9 and 14 (item 1 is the ASan `stack-overflow` that kills the browser). Items 3 and 6 were reproduced with separate runs of the PR build.
  - (Run logs were kept out of the repository.)
- The DDG-like flow passes: POST search → results → interstitial (meta + JS) → direct uddg → JS-only redirect → Back from cache → dark mode persisted. The dark-mode setting was written under a deep, non-ASCII `XDG_CONFIG_HOME`.
- **Live DuckDuckGo is not reachable from the box.** At 9:02 AM PT, `https://lite.duckduckgo.com` and `https://duckduckgo.com` returned no connection (TLS EOF), and plain HTTP returned 500. The mock reproduces DDG Lite's form and its `/l/?uddg=` interstitial markup.

## New efficiency suggestions (not in AUDIT.md)
1. **Fuzz the parsers.** Write libFuzzer harnesses for `prepare_html`, `parse_meta_refresh`, `scan_js_location`, `apply_dark_theme` and `resolve_url`, and run them for 60 s in CI. That class of tool would have found items 1, 10 and 22 automatically.
2. **Keep the cache cheaper:**
   - Cache the dark-themed HTML per `Page`. Back/Fwd in dark mode currently re-runs `apply_dark_theme` on every visit.
   - Compress cached pages that aren't on screen with zlib, which libcurl already links. HTML compresses 5–10×, so the 48 MB budget would hold far more history.
3. **Conditional reloads.** Store `ETag` / `Last-Modified` in `Page` and send `If-None-Match` / `If-Modified-Since` on reload. On a 304, reuse the cached page: no download and no parse.
4. **Truncate instead of failing.** Above about 2 MB of HTML, render the first part with a "page truncated" banner instead of the 8 MB hard error. `Fl_Help_View` layout is super-linear, so huge documents freeze it anyway.
5. **Parse while downloading.** Sniff the charset, BOM and meta refresh from the first chunk in the write callback. That allows aborting early for redirect pages and avoids holding raw, UTF-8 and prepared copies at the same time.
6. **Cheaper tables.** Replace the `std::map` entity table with a sorted `constexpr` array plus binary search (no static initialisation, no allocations). Store links as offsets into one buffer instead of `vector<string>`.

## New feature ideas (not in AUDIT.md)
1. **Lite-mirror routing.** Optionally rewrite heavy sites to their text front-ends, which suit `Fl_Help_View`:
   - old.reddit.com
   - lite.cnn.com
   - text.npr.org
   - the mobile Wikipedia skin
   - DDG Lite for searches
2. **Link privacy and transparency:**
   - Strip tracking parameters (`utm_*`, `fbclid`, `gclid`, `mc_eid`) before navigating.
   - Show the *unwrapped* final target (uddg decoded) in the status bar on hover.
   - Record the redirect chain in `Page` and show it on an `about:redirects` style page.
3. **Keyword search from any form.** Right-click (or Ctrl+K) a page's search form, give it a keyword, and typing `w python` in the URL bar submits that form directly. This builds on the parsed `Form` data. (Different from AUDIT.md's "search from the address bar", which only uses one fixed engine.)
4. **Submit preview.** An optional status-bar panel shows exactly what a form will send (method, URL, Content-Type, body) before it goes out. Useful for privacy and for learning HTML.
5. **Download non-HTML content** to `~/Downloads`, with progress from the existing progress callback, instead of the "cannot display" error page.
6. **Theme follows the system.** Detect the system theme (GTK `gtk-application-prefer-dark-theme`, or the Windows `AppsUseLightTheme` registry key), with per-site light or dark exceptions stored in the now-preserving config file.
7. **Session restore.** Reopen the last tab's history after a crash or restart, written with the new atomic config writer.

## Follow-up: JS-only controls flooded the form bar (FIX(3))

**Report:** on https://www.w3schools.com/python/ the PR build filled the form bar with 3 rows of widgets (search box, filter boxes, × buttons, "Back to topics", w3schools' own dark-mode checkbox) and the page with `[Filter...: ____] [ × ]` placeholders and broken-image icons.

**Root cause:** that page has **no `<form>` at all**. `prepare_html()` put every control outside a form into one implicit GET form: 17 JS-only controls (8 inputs, 9 buttons), most of them inside dropdown menus that CSS hides. In addition, all 34 `<img>` tags were drawn as broken images.

**Fix (`// FIX(3):` in `litebird.cpp`):**
- **No implicit form.** Controls without a form owner are dropped, unless `form="id"` names a real form. Without JS they can't submit anything.
- **Hidden content is skipped** (`tag_hides()`): text, links and controls are all dropped for:
  - the `hidden` attribute, `aria-hidden="true"`, `<template>` and closed `<dialog>`
  - inline `display:none` or `visibility:hidden`
  - exact hide-utility classes (`w3-hide`, `d-none`, `hidden`, `sr-only`, …; `w3-hide-small` stays visible)
  - `<nav>` dropdown, mobile, modal or off-canvas overlays

  Hidden controls inside real forms are still submitted with their default values. An unclosed hidden element doesn't hide the rest of the page.
- **Only forms that can be submitted are shown:** a form must have a visible text field, select or submit button.
- **The form bar shows one form at a time:** the `autofocus` form, else the first form with a text field. Clicking a placeholder of another form switches the bar to that form, and edits are kept in the page's form data. If the page has no such form, the bar collapses.
- **Placeholders** are spliced in only for rendered controls. `<label>` text of dropped controls is removed too.
- **Images:** `<img>` becomes `[alt text]` (at most 40 characters), or nothing when alt is empty, so there are no red-X icons. `<svg>`/`<math>` are dropped, and so are icon-font Private Use Area glyphs (U+E000–U+F8FF, as UTF-8 or `&#x…;`).

**Result on the real w3schools Python page (saved copy, 325 KB):**

| | Before | After |
|---|---|---|
| Implicit forms | 1 | 0 |
| Controls | 17 | 0 |
| Placeholders | 17 | 0 |
| `<img>` | 34 | 0 |
| Prepared HTML | 213 KB | 91 KB |
| Form bar | 3 rows | none |

The tutorial content and the sidebar links are unchanged.

## Files
- `litebird.cpp`: corrected source, with FIX(2) / CHANGED(2) markers
- `tests/test_helpers.cpp`: extended unit tests (`make test`)
- `tests/gui/gui_smoke.sh` and `tests/gui/mock_server.py`: the GUI smoke test (Xvfb + xdotool) and the DDG-like mock server
- `Makefile`, `README.md`: small fixes (items 35 and 36)

The sanitizer run logs and before/after screenshots referenced above were kept out of the repository.
