#!/bin/bash
# FEAT(5) GUI test: built-in home page (Xvfb + xdotool).
# usage: home_page.sh BIN TAG   (mock server must run on 18766, see gui_smoke.sh)
# The home page's DuckDuckGo Lite / wiby.me actions are pointed at the mock
# via LITEBIRD_DDG_LITE_URL / LITEBIRD_WIBY_URL.
BIN=$1; TAG=$2; B=http://127.0.0.1:18766
LOG=/tmp/mock2.log; ERR=/tmp/home-$TAG.err; SHOTS=/workspace/litebird-audit-2/screens
export XDG_CONFIG_HOME=/tmp/lbcfg-home-$TAG; rm -rf "$XDG_CONFIG_HOME"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
export LITEBIRD_DEBUG_SCROLL=1 LITEBIRD_DDG_LITE_URL=$B/lite/ LITEBIRD_WIBY_URL=$B/wiby/
W=; fails=0
title(){ xdotool getwindowname $W 2>/dev/null || echo "<no window>"; }
mark(){ OFF=$(stat -c %s $LOG); }
since(){ tail -c +$((OFF+1)) $LOG; }
check(){ if [ "$2" = "$3" ]; then echo "PASS $1: $2"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails+1)); fi; }
req(){ since | grep -o "GET $1[^ ]*" | head -1; }
H="LiteBird Home - LiteBird"
mark
$BIN --light > $ERR 2>&1 & P=$!
sleep ${SLEEP0:-3}; W=$(xdotool search --name "LiteBird" | head -1)
# 1. no URL argument: about:home, no network request, the page (not a field) has the focus
check "startup page" "$(title)" "$H"
check "startup requests" "$(since | grep -c 'GET ')" "0"
check "startup focus" "$(grep -o 'LB_FOCUS shown focus=[a-z]*' $ERR | tail -1)" "LB_FOCUS shown focus=view"
xdotool key Page_Down; sleep 0.4
check "PageDown on home" "$([ "$(grep -o 'topline=[0-9]*' $ERR | tail -1 | cut -d= -f2)" -gt 0 ] && echo scrolled)" "scrolled"
xdotool key Home; sleep 0.3
import -window root -crop 920x710+0+0 $SHOTS/$TAG-home-light.png 2>/dev/null
# 2. DuckDuckGo Lite from the form bar (the bar starts with the DDG form)
mark; xdotool mousemove 250 88 click 1; xdotool type "lbtest"; xdotool key Return; sleep ${LOADWAIT:-1.5}
check "DDG form (Enter)" "$(req /lite/)" "GET /lite/?q=lbtest"
check "DDG results" "$(title)" "lbtest at DDG GET - LiteBird"
# 3. Alt+Home, then the wiby form via its inline placeholder (switches the bar)
xdotool key alt+Home; sleep 0.8; check "Alt+Home" "$(title)" "$H"
mark; xdotool mousemove ${WIBY_X:-70} ${WIBY_Y:-331} click 1; sleep 0.5; xdotool type "wibytest"; xdotool key Return; sleep ${LOADWAIT:-1.5}
check "wiby form (placeholder + Enter)" "$(req /wiby/)" "GET /wiby/?q=wibytest"
check "wiby results" "$(title)" "wibytest at wiby - LiteBird"
# 4. Home button, Back / Forward through about:home (page cache)
xdotool mousemove 37 52 click 1; sleep 0.8; check "Home button" "$(title)" "$H"
mark; xdotool mousemove 757 52 click 1; sleep 0.8; check "Back from home" "$(title)" "wibytest at wiby - LiteBird"
check "Back served from cache" "$(since | grep -c 'GET ')" "0"
xdotool mousemove 817 52 click 1; sleep 0.8; check "Forward to home" "$(title)" "$H"
# 5. DDG submit button in the form bar
mark; xdotool mousemove 250 88 click 1; xdotool type "btn"; xdotool mousemove 490 88 click 1; sleep ${LOADWAIT:-1.5}
check "DDG submit button" "$(req /lite/)" "GET /lite/?q=btn"
# 6. words in the address bar search DuckDuckGo Lite; about:home / litebird:home typed
mark; xdotool key ctrl+l ctrl+a; xdotool type --delay 5 "hello world"; xdotool key Return; sleep ${LOADWAIT:-1.5}
check "address-bar search" "$(req /lite/)" "GET /lite/?q=hello+world"
xdotool key ctrl+l ctrl+a; xdotool type --delay 5 "about:home"; xdotool key Return; sleep 0.8; check "typed about:home" "$(title)" "$H"
xdotool key ctrl+l ctrl+a; xdotool type --delay 5 "litebird:home"; xdotool key Return; sleep 0.8; check "typed litebird:home" "$(title)" "$H"
# 7. reload of the home page needs no network
mark; xdotool key F5; sleep 0.8; check "reload home" "$(title) / $(since | grep -c 'GET ')" "$H / 0"
# 8. dark mode
xdotool key ctrl+d; sleep 1; import -window root -crop 920x710+0+0 $SHOTS/$TAG-home-dark.png 2>/dev/null; xdotool key ctrl+d; sleep 0.5
kill -0 $P 2>/dev/null && echo "browser alive" || { echo "browser DEAD"; fails=$((fails+1)); }
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "sanitizer reports: $(grep -c -E 'ERROR: AddressSanitizer|runtime error|ThreadSanitizer' $ERR)"
grep -m3 -E 'ERROR: AddressSanitizer|runtime error|WARNING: ThreadSanitizer|SEGV' $ERR
echo "home checks failed: $fails"
