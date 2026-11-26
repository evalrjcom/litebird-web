#!/bin/bash
# GUI smoke test (Xvfb + xdotool). Start the mock first:
#   python3 tests/gui/mock_server.py 18766 > /tmp/mock2.log 2>&1 &
# then: tests/gui/gui_smoke.sh ./litebird new   (or an ASan/TSan build)
# usage: gui_smoke.sh BIN TAG   (mock server must run on 18766)
BIN=$1; TAG=$2; B=http://127.0.0.1:18766
LOG=/tmp/mock2.log; SHOTS=/workspace/litebird-audit-2/screens
export XDG_CONFIG_HOME=/tmp/lbcfg-$TAG/deep/ünï/cfg; rm -rf /tmp/lbcfg-$TAG
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
title(){ xdotool getwindowname $W 2>/dev/null || echo "<no window>"; }
alive(){ kill -0 $P 2>/dev/null && echo alive || echo DEAD; }
go(){ xdotool key ctrl+l; xdotool key ctrl+a; xdotool type --delay 5 "$1"; xdotool key Return; sleep ${2:-2}; }
mark(){ OFF=$(stat -c %s $LOG); }
since(){ tail -c +$((OFF+1)) $LOG; }
$BIN --light $B/lite/ > /tmp/gui7-$TAG.err 2>&1 & P=$!
sleep ${SLEEP0:-3}; W=$(xdotool search --name "LiteBird" | head -1)
# 1. DDG-like flow (search form POST, uddg/interstitial/JS redirects, Back cache)
xdotool mousemove 150 88 click 1; xdotool type "python"; xdotool key Return; sleep 2; echo "ddg search: $(title)"
xdotool mousemove 160 144 click 1; sleep 2; echo "ddg first (interstitial): $(title)"
xdotool key alt+Left; sleep 1; echo "ddg back cached: $(title)"
xdotool mousemove 140 188 click 1; sleep 2; echo "ddg second (uddg): $(title)"
xdotool key alt+Left; sleep 1
xdotool mousemove 120 232 click 1; sleep 2; echo "ddg third (js): $(title)"
# 2. duplicate <option> labels: default "B" must be submitted as k=b
mark dupsel; go $B/dupsel; xdotool mousemove 370 88 click 1; sleep 2; echo "dupsel: $(title) | $(since | grep -o 'GET /echo[^ ]*' | head -1)"
# 3. multipart form, textarea CRLF
mark multipart; go $B/multipart; xdotool mousemove 140 88 click 1; xdotool key Return; sleep 2
echo "multipart: $(title) | $(since | grep POSTBODY | head -1 | cut -c1-260)"
# 4. windows-1252 page: form submitted in cp1252
mark latin1; go $B/latin1; xdotool mousemove 140 88 click 1; xdotool key Return; sleep 2
echo "latin1: $(title) | $(since | grep -o 'GET /echo[^ ]*' | head -1)"
# 5. Enter in a field whose form action is invalid -> synchronous error page (UAF check)
go $B/badaction; xdotool mousemove 140 88 click 1; xdotool key Return; sleep 1; echo "badaction: $(title) $(alive)"
xdotool mousemove 330 88 click 1; sleep 1; echo "badaction button: $(title) $(alive)"
# 6. page load finishing while a <select> popup is open (UAF check)
go $B/choice; echo "choice: $(title)"
xdotool key ctrl+l; xdotool key ctrl+a; xdotool type --delay 5 "$B/slow"; xdotool key Return
sleep 0.3; xdotool mousemove 60 88 click 1; sleep 4; echo "during popup (load done): $(title) $(alive)"
import -window root -crop 920x500+0+0 $SHOTS/$TAG-choice-popup.png 2>/dev/null
xdotool key Escape; sleep 1.5; echo "after popup closed: $(title) $(alive)"
# 7. meta-refresh loop
go $B/loop1 3; echo "loop: $(title)"
# 8. huge JS literal on a near-empty page (std::regex stack overflow)
go $B/bigjs 3; echo "bigjs: $(title) $(alive)"
# 9. 'geolocation =' must not redirect
[ "$(alive)" = alive ] && { go $B/geo; echo "geo: $(title)"; }
# 10. dark mode persisted under a deep non-ASCII XDG_CONFIG_HOME
[ "$(alive)" = alive ] && { xdotool key ctrl+d; sleep 1; import -window root -crop 920x400+0+0 $SHOTS/$TAG-dark.png 2>/dev/null; }
echo "config: $(cat "$XDG_CONFIG_HOME/litebird/config" 2>/dev/null | tr '\n' ' ' || echo none)"
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "sanitizer reports: $(grep -c -E 'ERROR: AddressSanitizer|runtime error|ThreadSanitizer' /tmp/gui7-$TAG.err)"
grep -m3 -E 'ERROR: AddressSanitizer|runtime error|WARNING: ThreadSanitizer|SEGV' /tmp/gui7-$TAG.err
