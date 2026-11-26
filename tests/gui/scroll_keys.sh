#!/bin/bash
# FIX(4) GUI test: keyboard scrolling of the page view (Xvfb + xdotool).
# usage: scroll_keys.sh BIN TAG   (mock server must run on 18766, see gui_smoke.sh)
# The browser logs every scroll position ("LB_SCROLL topline=N") when
# LITEBIRD_DEBUG_SCROLL is set; each check presses a key and expects the
# top line to move in the right direction.
BIN=$1; TAG=$2; B=http://127.0.0.1:18766
ERR=/tmp/scroll-$TAG.err; SHOTS=/workspace/litebird-audit-2/screens
export XDG_CONFIG_HOME=/tmp/lbcfg-scroll-$TAG; rm -rf "$XDG_CONFIG_HOME"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
export LITEBIRD_DEBUG_SCROLL=1
tl(){ grep -o 'topline=[0-9]*' $ERR | tail -1 | cut -d= -f2; }
fails=0
# expect NAME up|down|top|bottom KEY-ACTION...
expect(){ local name=$1 dir=$2; shift 2; local a b; a=$(tl); "$@"; sleep ${STEP:-0.5}; b=$(tl); a=${a:-0}; b=${b:-0}
  local ok=0
  case $dir in down) [ "$b" -gt "$a" ] && ok=1;; up) [ "$b" -lt "$a" ] && ok=1;; top) [ "$b" -eq 0 ] && [ "$a" -gt 0 ] && ok=1;;
               bottom) [ "$b" -gt "$a" ] && [ "$b" -gt 2000 ] && ok=1;; esac
  if [ $ok = 1 ]; then echo "PASS $name ($a -> $b)"; else echo "FAIL $name ($a -> $b)"; fails=$((fails+1)); fi; }
key(){ xdotool key "$@"; }
load(){ xdotool key ctrl+l ctrl+a; xdotool type --delay 5 "$1"; xdotool key Return; sleep ${LOADWAIT:-1.5}; }
$BIN --light $B/long > $ERR 2>&1 & P=$!
sleep ${SLEEP0:-3}
# 1. after a load started with Enter in the address bar (focus was left in the URL field)
load $B/long2
expect "PageDown after URL-bar Enter" down key Page_Down
expect "Space"                        down key space
expect "Down arrow"                   down key Down
expect "PageUp"                       up   key Page_Up
expect "Shift+Space"                  up   key shift+space
expect "Up arrow"                     up   key Up
expect "End"                          bottom key End
expect "Home"                         top  key Home
expect "Ctrl+End"                     bottom key ctrl+End
expect "Ctrl+Home"                    top  key ctrl+Home
# 2. after clicking the Go button (toolbar buttons must not keep the focus)
xdotool mousemove ${GO_X:-697} ${BAR_Y:-52} click 1; sleep ${LOADWAIT:-1.5}
expect "PageDown after Go click"      down key Page_Down
# 3. after clicking a link in the page
key Home; sleep 0.3; xdotool mousemove ${LINK_X:-50} ${LINK_Y:-85} click 1; sleep ${LOADWAIT:-1.5}
echo "  link click -> $(xdotool getwindowname $(xdotool search --name LiteBird | head -1))"
expect "PageDown after link click"    down key Page_Down
# 4. after clicking the Back button (cached page)
xdotool mousemove ${BACK_X:-757} ${BAR_Y:-52} click 1; sleep 1
echo "  back -> $(xdotool getwindowname $(xdotool search --name LiteBird | head -1))"
expect "PageDown after Back click"    down key Page_Down
# 5. after clicking the theme button, and after clicking the page text
xdotool mousemove ${THEME_X:-880} ${BAR_Y:-52} click 1; sleep 0.8
expect "PageDown after Dark click"    down key Page_Down
xdotool mousemove ${THEME_X:-880} ${BAR_Y:-52} click 1; sleep 0.8
xdotool mousemove 400 400 click 1; sleep 0.3
expect "Down after clicking page text" down key Down
# 6. mouse wheel over the page text (not over the scrollbar)
xdotool mousemove 400 300
expect "wheel down over text"         down xdotool click 5
expect "wheel up over text"           up   xdotool click 4
# 7. page with an autofocus text field: PageDown scrolls, Space types into the field
load $B/longform
expect "PageDown in autofocus field"  down key Page_Down
a=$(tl); key space; sleep 0.4; b=$(tl)
if [ "$a" = "$b" ]; then echo "PASS Space types in the text field (no scroll: $a)"; else echo "FAIL Space scrolled from a text field ($a -> $b)"; fails=$((fails+1)); fi
# 8. typing in the address bar: Space/Down edit the URL, focus is not stolen
xdotool key ctrl+l; xdotool type --delay 5 "abc"; a=$(tl); key space Down; sleep 0.3; b=$(tl)
if [ "$a" = "$b" ]; then echo "PASS keys stay in the address bar while typing"; else echo "FAIL address bar typing scrolled ($a -> $b)"; fails=$((fails+1)); fi
# 9. clicking into the address bar while a page loads keeps the focus there
xdotool key Escape; load $B/long 1
xdotool key ctrl+l ctrl+a; xdotool type --delay 5 "$B/slow"; xdotool key Return; sleep 0.3
xdotool mousemove 300 ${BAR_Y:-52} click 1; sleep 3.5
f=$(grep -o 'LB_FOCUS shown focus=[a-z]*' $ERR | tail -1)
if [ "$f" = "LB_FOCUS shown focus=url" ]; then echo "PASS address bar keeps focus clicked during a load"; else echo "FAIL focus after slow load: $f"; fails=$((fails+1)); fi
import -window root -crop 920x680+0+0 $SHOTS/$TAG-scroll.png 2>/dev/null
kill -0 $P 2>/dev/null && echo "browser alive" || { echo "browser DEAD"; fails=$((fails+1)); }
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "sanitizer reports: $(grep -c -E 'ERROR: AddressSanitizer|runtime error|ThreadSanitizer' $ERR)"
grep -m3 -E 'ERROR: AddressSanitizer|runtime error|WARNING: ThreadSanitizer|SEGV' $ERR
echo "scroll checks failed: $fails"
