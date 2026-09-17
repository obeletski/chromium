"""Minimal DevTools (CDP) client: navigate a tab and dump its rendered DOM.

Two things make this necessary rather than a convenience.

* **`chrome://` pages cannot be opened the easy ways.** DevTools' `/json/new`
  endpoint refuses a `chrome://` URL outright, and `--headless` accepts only one
  target, so neither reaches `chrome://floating-window`. Attaching to an
  ordinary `about:blank` tab and driving `Page.navigate` does.
* **No websocket library is installed in this checkout**, and adding one is not
  worth it, so this speaks the slice of RFC6455 a client needs: an HTTP Upgrade
  handshake, masked text frames outbound, unmasked frames inbound. Client frames
  must be masked (RFC6455 5.3) or the server closes the connection.

This reads the DOM *after* `Page.loadEventFired` and via `Runtime.evaluate` of
`document.documentElement.outerHTML`, so it captures what the page's script
actually built, not the HTML the WebUI data source served.

Usage:
    python3 cdp.py <url> <out.html> [debug-port]     # default port 9222

The browser must have been started with `--remote-debugging-port=<port>`.
"""
import base64
import json
import os
import socket
import struct
import sys
import urllib.request


def ws_connect(url):
    # url looks like ws://127.0.0.1:9222/devtools/page/<id>
    rest = url[len("ws://"):]
    hostport, path = rest.split("/", 1)
    path = "/" + path
    host, port = hostport.split(":")
    s = socket.create_connection((host, int(port)), timeout=20)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(
        f"GET {path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    assert b"101" in buf.split(b"\r\n")[0], buf.split(b"\r\n")[0]
    # The server may coalesce the first data frame into the same TCP segment as
    # the handshake response, so hand the leftover bytes to the Reader.
    return s, buf.split(b"\r\n\r\n", 1)[1]


def send(s, obj):
    payload = json.dumps(obj).encode()
    n = len(payload)
    header = b"\x81"  # FIN + opcode 1 (text)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(header + mask + masked)


class Reader:
    """Reassembles inbound frames; skips anything that is not a text frame."""

    def __init__(self, sock, initial=b""):
        self.s, self.buf = sock, initial

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def frame(self):
        self._need(2)
        b1, b2 = self.buf[0], self.buf[1]
        ln, off = b2 & 0x7F, 2
        if ln == 126:
            self._need(4)
            ln = struct.unpack(">H", self.buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10)
            ln = struct.unpack(">Q", self.buf[2:10])[0]
            off = 10
        self._need(off + ln)
        data = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        return b1 & 0x0F, data

    def message(self):
        """Next text frame, decoded. CDP replies are never fragmented."""
        while True:
            op, data = self.frame()
            if op == 1:
                return json.loads(data)


def main():
    target_url, out_path = sys.argv[1], sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else "9222"

    targets = json.load(
        urllib.request.urlopen("http://127.0.0.1:%s/json/list" % port))
    # Prefer a blank tab: navigating one that already shows something would
    # destroy the state the screenshot half of the harness set up.
    page = next((t for t in targets
                 if t["type"] == "page" and t["url"].startswith("about:blank")),
                None)
    if page is None:
        page = next(t for t in targets if t["type"] == "page")
    sock, extra = ws_connect(page["webSocketDebuggerUrl"])
    r = Reader(sock, extra)

    send(sock, {"id": 1, "method": "Page.enable"})
    send(sock, {"id": 2, "method": "Page.navigate",
                "params": {"url": target_url}})
    seen_load = False
    while True:
        msg = r.message()
        if msg.get("method") == "Page.loadEventFired":
            seen_load = True
            break
        if msg.get("id") == 2 and "error" in msg:
            print("NAVIGATE ERROR:", msg["error"], file=sys.stderr)
            break

    send(sock, {"id": 3, "method": "Runtime.evaluate",
                "params": {"expression": "document.documentElement.outerHTML",
                           "returnByValue": True}})
    while True:
        msg = r.message()
        if msg.get("id") == 3:
            with open(out_path, "w") as f:
                f.write(msg["result"]["result"]["value"])
            print("wrote", out_path, "loaded=", seen_load)
            return


main()
