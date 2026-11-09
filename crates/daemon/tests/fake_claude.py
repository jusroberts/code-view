#!/usr/bin/env python3
"""Minimal stand-in for `claude -p --input-format stream-json ...` used by tests."""
import json, sys

args = sys.argv[1:]
if args[:2] == ["auth", "status"]:
    print(json.dumps({"loggedIn": True, "authMethod": "fake"}))
    sys.exit(0)

sid = args[args.index("--session-id") + 1] if "--session-id" in args else args[args.index("--resume") + 1]

mode = args[args.index("--permission-mode") + 1] if "--permission-mode" in args else "none"

def out(o):
    sys.stdout.write(json.dumps(o) + "\n")
    sys.stdout.flush()

def read():
    line = sys.stdin.readline()
    return json.loads(line) if line else None

while True:
    msg = read()
    if msg is None:
        break
    if msg["type"] == "control_request":
        req = msg["request"]
        if req["subtype"] == "set_permission_mode":
            if req["mode"] not in ("manual", "default", "acceptEdits", "auto", "plan"):  # like claude: bypass needs the launch flag
                out({"type": "control_response", "response": {"subtype": "error", "request_id": msg["request_id"], "error": f"can't set {req['mode']}"}})
                continue
            mode = req["mode"]
        out({"type": "control_response", "response": {"subtype": "success", "request_id": msg["request_id"], "response": {}}})
        continue
    content = msg["message"]["content"]
    text = content[0]["text"]
    images = sum(1 for c in content if c["type"] == "image")
    out({"type": "system", "subtype": "init", "session_id": sid, "model": "fake-resumed" if "--resume" in args else "fake-new"})
    out({"type": "stream_event", "event": {"type": "content_block_delta", "delta": {"type": "text_delta", "text": "Hel"}}})
    out({"type": "assistant", "message": {"content": [{"type": "text", "text": f"echo:{text} images:{images}"}]}})
    if text == "mode?":
        out({"type": "assistant", "message": {"content": [{"type": "text", "text": f"mode:{mode}"}]}})
    if text == "run tool":
        out({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": "ls"}}]}})
        out({"type": "control_request", "request_id": "perm1", "request": {"subtype": "can_use_tool", "tool_name": "Bash", "input": {"command": "ls"}}})
        reply = read()
        behavior = reply["response"]["response"]["behavior"]
        out({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t1", "content": behavior, "is_error": behavior != "allow"}]}})
    if text.startswith("edit "):  # "edit PATH": append a line to PATH, reported as an Edit tool call
        path = text[5:]
        with open(path, "a") as f:
            f.write("edited by fake claude\n")
        out({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "e1", "name": "Edit", "input": {"file_path": path}}]}})
        out({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "e1", "content": "ok", "is_error": False}]}})
    out({"type": "result", "subtype": "success", "is_error": False, "duration_ms": 5, "total_cost_usd": 0.0})
