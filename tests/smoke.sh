#!/bin/zsh
# release smoke test: launches macVC, waits for the loader and plugins to
# start, runs a few console commands through MVC_EXEC, checks the log for
# errors and new crash reports, then quits the game. needs a macVC with the
# loader installed and nobody playing it. only gets to the main menu, and
# never sends the game any input.
#   tests/smoke.sh [path/to/macVC.app]
cd "${0:A:h}/.."
APP=${1:-${MACVC_APP:-$HOME/Desktop/macVC.app}}
GD="$APP/Contents/Resources/GameData"
LOG="$GD/macVCLoader.log"
[[ -d $GD ]] || { echo "no macVC at $APP"; exit 2; }
if pgrep -x macVC >/dev/null; then echo "macVC is running - quit it first (the smoke test launches its own)"; exit 2; fi

crashes_before=$(ls "$GD/crashes" 2>/dev/null | wc -l)
rm -f "$LOG"   # otherwise the wait below reads the previous session's log
open -a "$APP" --env MVC_EXEC="echo SMOKE-BEGIN;plugins;hooks;cleo;mods;perf;find SMOKE;find;echo SMOKE-DONE"

fail=0
for i in {1..180}; do
  [[ -f $LOG ]] && grep -q "SMOKE-DONE" "$LOG" && break
  if (( i > 10 )) && ! pgrep -x macVC >/dev/null; then echo "FAIL: the game quit during startup"; fail=1; break; fi
  sleep 1
done
(( fail )) || grep -q "SMOKE-DONE" "$LOG" || { echo "FAIL: console commands never ran (no SMOKE-DONE within 180 s)"; fail=1; }

osascript -e 'tell application "macVC" to quit' >/dev/null 2>&1   # prints "User canceled" even when it quits
for i in {1..20}; do pgrep -x macVC >/dev/null || break; sleep 0.5; done
pgrep -x macVC >/dev/null && { echo "note: macVC didn't quit; closing it"; pkill -x macVC; }

if [[ -f $LOG ]]; then
  echo "--- summary"
  grep -E "^\[Loader\] (macVC Plugin Loader|[0-9]+ plugin\(s\) loaded)|^\[CLEO\] runner ready|^\[ModLoader\] [0-9]+ mod\(s\)" "$LOG"
  grep -qE "^\[Loader\] macVC Plugin Loader" "$LOG" || { echo "FAIL: the loader never started"; fail=1; }
  # the startup summary ("3 plugin(s) loaded, 0 disabled, 0 failed"), not the `plugins` command's header
  grep -E "^\[Loader\] [0-9]+ plugin\(s\) loaded, " "$LOG" | grep -qv " 0 failed" && { echo "FAIL: a plugin failed to load"; fail=1; }
  errs=$(grep -E "^\[[A-Za-z]+\] (ERROR|.*ERROR:)" "$LOG")
  [[ -n $errs ]] && { echo "FAIL: errors in the log:"; echo "$errs"; fail=1; }
  grep -q "^> plugins" "$LOG" || { echo "FAIL: the console didn't echo commands"; fail=1; }
fi
crashes_after=$(ls "$GD/crashes" 2>/dev/null | wc -l)
(( crashes_after > crashes_before )) && { echo "FAIL: new crash report in $GD/crashes"; fail=1; }

(( fail )) && { echo "SMOKE TEST FAILED (full log: $LOG)"; exit 1; }
echo "smoke test passed"
