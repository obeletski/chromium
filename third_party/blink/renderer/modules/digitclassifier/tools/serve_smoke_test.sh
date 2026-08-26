#!/bin/bash
# Serve tools/smoke_test.html from the *device's own* 127.0.0.1, which is a
# secure context (the API is [SecureContext]).
#
# Why not simply `adb reverse` to a host server: when adb talks to a remote adb
# server over an SSH tunnel, `adb reverse`/`adb forward` bind ports on the
# machine running the adb *server*, not this one. Serving from the device
# sidesteps host<->device reachability entirely.
#
# Usage: serve_smoke_test.sh <device-serial> [port]
set -euo pipefail
SERIAL="$1"; PORT="${2:-8111}"
SRC="$(dirname "$0")"
ADB="$(cd "$SRC/../../../../../.." && pwd)/third_party/android_sdk/public/platform-tools/adb"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# toybox nc cannot speak HTTP, so hand it a complete canned response.
python3 - "$SRC/smoke_test.html" "$TMP/resp.http" <<'PY'
import sys
body = open(sys.argv[1], "rb").read()
hdr = ("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
       f"Content-Length: {len(body)}\r\nCache-Control: no-store\r\n"
       "Connection: close\r\n\r\n").encode()
open(sys.argv[2], "wb").write(hdr + body)
PY

"$ADB" -s "$SERIAL" reverse --remove-all 2>/dev/null || true   # frees the port on the device
"$ADB" -s "$SERIAL" push "$TMP/resp.http" /data/local/tmp/resp.http >/dev/null
"$ADB" -s "$SERIAL" shell chmod 644 /data/local/tmp/resp.http
printf '#!/system/bin/sh\nexec nc -4 -L -s 127.0.0.1 -p %s /system/bin/sh -c "cat /data/local/tmp/resp.http"\n' \
    "$PORT" > "$TMP/serve.sh"
"$ADB" -s "$SERIAL" push "$TMP/serve.sh" /data/local/tmp/serve.sh >/dev/null
"$ADB" -s "$SERIAL" shell chmod 755 /data/local/tmp/serve.sh

# The listener dies with its adb shell, so hold the connection open.
"$ADB" -s "$SERIAL" shell /data/local/tmp/serve.sh &
sleep 2
"$ADB" -s "$SERIAL" shell "printf 'GET / HTTP/1.1\r\n\r\n' | nc -4 127.0.0.1 $PORT | head -1"
echo "serving on device at http://127.0.0.1:$PORT/ (ctrl-c to stop)"
wait
