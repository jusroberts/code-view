#!/usr/bin/env python3
"""Tiny JSON-RPC-over-stdio server: answers every request with {"echo": method}."""
import json, sys

def read():
    length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(sys.stdin.buffer.read(length))

def send(obj):
    body = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()

while (msg := read()) is not None:
    if "id" in msg:
        send({"jsonrpc": "2.0", "id": msg["id"], "result": {"echo": msg["method"]}})
