#!/usr/bin/env python3
"""Pretends to be the PS4 plugin so you can try the PC app without a console.

    python mock_ps4.py                 # serves on port 8085
    python mock_ps4.py --simulate-game # also fakes a game reading/writing figures

The Portal class is a line-by-line Python port of ps4_plugin/source/portal.c,
so it doubles as a reference for the protocol and is used by the tests.
"""
import argparse
import json
import os
import re
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SLOTS = 8
FIGURE_SIZE = 1024
REPORT = 32
HERE = os.path.dirname(os.path.abspath(__file__))


class Portal:
    def __init__(self):
        self.lock = threading.Lock()
        self.slots = [dict(data=bytearray(FIGURE_SIZE), loaded=False, dirty=False, name="",
                           status=0, queued=[]) for _ in range(SLOTS)]
        self.queue = []
        self.active = False
        self.counter = 0

    def _reply(self, r):
        if len(self.queue) == 32:
            self.queue.pop(0)
        self.queue.append(bytes(r).ljust(REPORT, b"\0")[:REPORT])

    def _status(self):
        bits = 0
        for s in reversed(self.slots):
            if s["queued"]:
                s["status"] = s["queued"].pop(0)
            bits = (bits << 2) | (s["status"] & 3)
        out = bytearray(REPORT)
        out[0] = ord("S")
        out[1:5] = bits.to_bytes(4, "little")
        out[5] = self.counter
        out[6] = 1 if self.active else 0
        self.counter = (self.counter + 1) & 0xFF
        return bytes(out)

    # game side
    def command(self, buf):
        with self.lock:
            c = chr(buf[0])
            if c == "A":
                self.active = bool(buf[1])
                self._reply([ord("A"), buf[1], 0xFF, 0x77])
            elif c == "J":
                self._reply([ord("J")])
            elif c == "M":
                self._reply([ord("M"), buf[1], 0x00, 0x19])
            elif c == "Q":
                idx, block = buf[1] & 0xF, buf[2]
                s = self.slots[idx] if idx < SLOTS else None
                if s and s["loaded"] and block < 64:
                    self._reply(bytes([ord("Q"), 0x10 | idx, block]) + s["data"][block * 16:block * 16 + 16])
                else:
                    self._reply([ord("Q"), idx, block])   # real portal: 51 0<slot> <block>
            elif c == "R":
                self._reply([ord("R"), 0x02, 0x03])
            elif c == "S":
                self._reply(self._status())
            elif c == "W":
                idx, block = buf[1] & 0xF, buf[2]
                s = self.slots[idx] if idx < SLOTS else None
                if s and s["loaded"] and block < 64:
                    s["data"][block * 16:block * 16 + 16] = buf[3:19]
                    s["dirty"] = True
                    self._reply([ord("W"), 0x10 | idx, block])
                else:
                    self._reply([ord("W"), 0x01, block])

    def report(self):
        with self.lock:
            if self.queue:
                return self.queue.pop(0), True
            return self._status(), False

    # PC side
    def load(self, slot, data, name):
        with self.lock:
            s = self.slots[slot]
            s["data"][:] = data[:FIGURE_SIZE]
            s["loaded"], s["dirty"], s["name"] = True, False, name[:63]
            s["queued"] = [2, 0, 3, 1] if s["status"] & 1 else [3, 1]

    def remove(self, slot):
        with self.lock:
            s = self.slots[slot]
            if not s["loaded"]:
                return False
            s["loaded"] = False
            s["queued"] = [2, 0]
            return True

    def info(self):
        with self.lock:
            out = []
            for i, s in enumerate(self.slots):
                d = s["data"]
                out.append(dict(slot=i, loaded=s["loaded"], dirty=s["dirty"],
                                id=int.from_bytes(d[0x10:0x12], "little") if s["loaded"] else 0,
                                variant=int.from_bytes(d[0x1C:0x1E], "little") if s["loaded"] else 0,
                                name=s["name"] if s["loaded"] else ""))
            return dict(active=self.active, slots=out)


def load_index_html():
    """Uses the exact web page compiled into the plugin."""
    try:
        with open(os.path.join(HERE, "..", "ps4_plugin", "source", "http.c"), encoding="utf-8") as f:
            src = f.read()
        block = re.search(r"INDEX_HTML\[\]\s*=\s*(.*?);\n", src, re.S).group(1)
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
        return "".join(p.encode().decode("unicode_escape") for p in parts).encode()
    except Exception:
        return b"<h1>mock portal</h1>"


def make_handler(portal, index_html):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *args):
            print("[http]", fmt % args)

        def _send(self, code, body, ctype="application/json"):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        def _slot(self, q):
            try:
                s = int(q.get("slot", [""])[0])
                return s if 0 <= s < SLOTS else None
            except ValueError:
                return None

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path == "/":
                self._send(200, index_html, "text/html")
            elif u.path == "/api/status":
                self._send(200, portal.info())
            elif u.path == "/api/dump":
                s = self._slot(q)
                if s is None or not portal.slots[s]["name"]:
                    self._send(404, {"error": "no figure in that slot"})
                else:
                    self._send(200, bytes(portal.slots[s]["data"]), "application/octet-stream")
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            n = int(self.headers.get("Content-Length") or 0)
            if n > FIGURE_SIZE * 4:
                self._send(413, {"error": "file too large"})
                return
            body = self.rfile.read(n) if n else b""
            s = self._slot(q)
            if u.path == "/api/load":
                if s is None:
                    self._send(400, {"error": "bad slot"})
                elif len(body) < FIGURE_SIZE:
                    self._send(400, {"error": "dump must be at least 1024 bytes"})
                else:
                    portal.load(s, body, q.get("name", ["figure.dump"])[0])
                    self._send(200, {"ok": True})
            elif u.path == "/api/remove":
                if s is None:
                    self._send(400, {"error": "bad slot"})
                else:
                    portal.remove(s)
                    self._send(200, {"ok": True})
            else:
                self._send(404, {"error": "not found"})

    return Handler


def simulate_game(portal):
    """Acts like the game: activates the portal, reads new figures, writes one block."""
    portal.command(b"R" + bytes(31))
    portal.command(b"A\x01" + bytes(30))
    present = [False] * SLOTS
    while True:
        rep, is_reply = portal.report()
        if not is_reply and rep[0] == ord("S"):
            bits = int.from_bytes(rep[1:5], "little")
            for i in range(SLOTS):
                st = (bits >> (2 * i)) & 3
                if st & 1 and not present[i]:
                    present[i] = True
                    for b in range(64):
                        portal.command(bytes([ord("Q"), 0x10 | i, b]) + bytes(29))
                    # "level up": write something into block 8
                    portal.command(bytes([ord("W"), 0x10 | i, 8]) + b"GAME-WAS-HERE!!!" + bytes(13))
                    print(f"[game] read figure in slot {i + 1} and wrote block 8")
                elif not st & 1 and present[i]:
                    present[i] = False
                    print(f"[game] figure left slot {i + 1}")
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8085)
    ap.add_argument("--simulate-game", action="store_true")
    a = ap.parse_args()
    portal = Portal()
    if a.simulate_game:
        threading.Thread(target=simulate_game, args=(portal,), daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), make_handler(portal, load_index_html()))
    print(f"Mock PS4 portal on http://127.0.0.1:{a.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
