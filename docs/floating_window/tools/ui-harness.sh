#!/bin/bash
# Screenshots browser-process UI: runs this checkout's chrome on a virtual X
# display, drives it with synthetic hardware input, and captures the screen.
#
# `chrome --headless --screenshot` photographs the *page*. A toolbar button, a
# bubble, a menu -- anything owned by the browser process rather than a renderer
# -- does not exist there, which is why this exists instead.
#
# Usage:
#   docs/floating_window/tools/ui-harness.sh \
#       --page file:///tmp/a.html --page file:///tmp/b.html \
#       --shot before --step click:1325,83 --shot open \
#       --step key:Escape --shot esc
#
# Options, applied in the order given (so --shot/--step interleave):
#   --page URL     open URL in the first window (repeatable)
#   --window URL   open URL in an additional browser window (repeatable)
#   --shot NAME    scrot the whole display to $WORK/NAME.png
#   --step CMD     hand CMD to xinput.py (click:X,Y | key:NAME | sleep:N | move:X,Y)
#   --sleep N      wait N seconds
#
# Environment:
#   OUT_DIR  build dir holding chrome          (default out/Linux)
#   WORK     where the profile and PNGs land   (default a fresh mktemp -d)
#   DISPLAY_NUM, SCREEN, WINDOW, SETTLE, PORT  (see below)
#
# Nothing is written into the tree: point WORK at a scratch directory and read
# the PNGs from there.
set -euo pipefail

OUT_DIR="${OUT_DIR:-out/Linux}"
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CHROME="$SRC_ROOT/$OUT_DIR/chrome"
WORK="${WORK:-$(mktemp -d)}"
DISPLAY_NUM="${DISPLAY_NUM:-:99}"
SCREEN="${SCREEN:-1500x950x24}"
WINDOW="${WINDOW:-1400,900}"
# A debug/component build reaches a painted toolbar many seconds after the
# process starts. Screenshotting early yields a blank or half-drawn window, and
# the icon-finder then reports "no dark pixels" rather than anything useful.
SETTLE="${SETTLE:-14}"
PORT="${PORT:-9222}"

[ -x "$CHROME" ] || { echo "no chrome at $CHROME; build it first" >&2; exit 1; }
for bin in Xvfb scrot; do
  command -v "$bin" >/dev/null || { echo "$bin not installed" >&2; exit 1; }
done

mkdir -p "$WORK"
PROFILE="$WORK/profile"
rm -rf "$PROFILE"

# Kill by profile path, never by process name: more than one Claude session can
# share this checkout, and `pkill chrome` would take out someone else's run.
cleanup() {
  pkill -f "user-data-dir=$PROFILE" 2>/dev/null || true
  sleep 1
  [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

pkill -f "user-data-dir=$PROFILE" 2>/dev/null || true
Xvfb "$DISPLAY_NUM" -screen 0 "$SCREEN" >"$WORK/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 3
export DISPLAY="$DISPLAY_NUM"

# --no-sandbox because the harness usually runs without a user namespace;
# --disable-dev-shm-usage because a small /dev/shm crashes the renderer with an
# error that reads like a GPU problem.
CHROME_ARGS=(--user-data-dir="$PROFILE" --no-first-run --no-default-browser-check
             --disable-sync --no-sandbox --disable-dev-shm-usage
             --window-size="$WINDOW" --remote-debugging-port="$PORT")

# Pass 1: collect the pages for the first window, since chrome takes them as
# positional arguments and must be launched once, before any step runs.
PAGES=()
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  if [ "${args[$i]}" = "--page" ]; then PAGES+=("${args[$((i + 1))]}"); fi
done

"$CHROME" "${CHROME_ARGS[@]}" "${PAGES[@]}" >"$WORK/chrome.log" 2>&1 &
sleep "$SETTLE"

# Pass 2: everything else, in the order written.
i=0
while [ $i -lt ${#args[@]} ]; do
  case "${args[$i]}" in
    --page)   i=$((i + 2));;  # already launched above
    --window)
      # A second `chrome` against a live profile is a launcher: it hands the URL
      # to the running browser and exits, which is how a second window is made.
      "$CHROME" "${CHROME_ARGS[@]}" --new-window "${args[$((i + 1))]}" \
          >>"$WORK/chrome.log" 2>&1 || true
      sleep 6
      i=$((i + 2));;
    --shot)
      scrot -o "$WORK/${args[$((i + 1))]}.png"
      echo "shot  $WORK/${args[$((i + 1))]}.png"
      i=$((i + 2));;
    --step)
      python3 "$SRC_ROOT/docs/floating_window/tools/xinput.py" "${args[$((i + 1))]}"
      i=$((i + 2));;
    --sleep)
      sleep "${args[$((i + 1))]}"
      i=$((i + 2));;
    *) echo "unknown option: ${args[$i]}" >&2; exit 1;;
  esac
done

echo
echo "--- md5 of captures (identical shots prove a state was restored) ---"
md5sum "$WORK"/*.png 2>/dev/null || true
echo
echo "--- failures in chrome.log ---"
grep -iE "FATAL|DCHECK|Content Security Policy|ERROR:CONSOLE" "$WORK/chrome.log" \
  | head -20 || echo "(none)"
echo
echo "artifacts in $WORK"
