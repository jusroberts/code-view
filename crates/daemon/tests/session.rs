//! End-to-end daemon tests against a fake `claude` (tests/fake_claude.py).

use serde_json::{Value, json};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

struct Daemon {
    child: Child,
    socket: PathBuf,
}

impl Daemon {
    fn start(dir: &Path) -> Daemon {
        let socket = dir.join("sock/daemon.sock");
        // A login shell whose PATH includes dir/late, which the daemon's own PATH doesn't.
        let shell = dir.join("fake-shell");
        std::fs::write(&shell, format!("#!/bin/sh\necho \"PATH={}/late:$PATH\"\n", dir.display())).unwrap();
        std::fs::set_permissions(&shell, std::os::unix::fs::PermissionsExt::from_mode(0o755)).unwrap();
        let child = Command::new(env!("CARGO_BIN_EXE_codeviewd"))
            .env("CODEVIEW_SOCKET", &socket)
            .env("CODEVIEW_STATE_DIR", dir.join("state"))
            .env("CLAUDE_CONFIG_DIR", dir.join("claude"))
            .env("SHELL", &shell)
            .env("CODEVIEW_CLAUDE_BIN", concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fake_claude.py"))
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(10);
        while UnixStream::connect(&socket).is_err() {
            assert!(Instant::now() < deadline, "daemon did not start");
            std::thread::sleep(Duration::from_millis(50));
        }
        Daemon { child, socket }
    }
}

impl Drop for Daemon {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

struct Client {
    w: UnixStream,
    r: BufReader<UnixStream>,
    next_id: u64,
    events: Vec<Value>,
}

impl Client {
    fn connect(d: &Daemon) -> Client {
        let w = UnixStream::connect(&d.socket).unwrap();
        w.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        let r = BufReader::new(w.try_clone().unwrap());
        Client { w, r, next_id: 1, events: Vec::new() }
    }

    fn read(&mut self) -> Value {
        let mut line = String::new();
        self.r.read_line(&mut line).expect("timed out waiting for daemon");
        serde_json::from_str(&line).unwrap()
    }

    /// Sends a request and returns its `data`, collecting events seen meanwhile.
    fn call(&mut self, request: Value) -> Result<Value, String> {
        let id = self.next_id;
        self.next_id += 1;
        writeln!(self.w, "{}", json!({"id": id, "request": request})).unwrap();
        loop {
            let msg = self.read();
            if msg["kind"] == "response" && msg["id"] == id {
                return if msg["ok"] == true { Ok(msg["data"].clone()) } else { Err(msg["error"].to_string()) };
            }
            self.events.push(msg);
        }
    }

    /// Waits for a session event matching `pred`, returning it.
    fn wait_event(&mut self, pred: impl Fn(&Value) -> bool) -> Value {
        if let Some(i) = self.events.iter().position(|m| m["kind"] == "event" && m["event"]["type"] == "session" && pred(&m["event"]["event"])) {
            return self.events.remove(i)["event"]["event"].clone();
        }
        loop {
            let msg = self.read();
            if msg["kind"] == "event" && msg["event"]["type"] == "session" && pred(&msg["event"]["event"]) {
                return msg["event"]["event"].clone();
            }
            self.events.push(msg);
        }
    }
}

fn tempdir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("codeviewd-test-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn chat_permission_and_resume() {
    let dir = tempdir("chat");
    let project = dir.join("project");
    std::fs::create_dir_all(&project).unwrap();
    std::fs::write(project.join("a.txt"), "hello").unwrap();

    let sid;
    {
        let d = Daemon::start(&dir);
        let mut c = Client::connect(&d);
        assert_eq!(c.call(json!({"op": "hello"})).unwrap()["protocol"], 1);
        assert_eq!(c.call(json!({"op": "auth_status"})).unwrap()["loggedIn"], true);

        let info = c.call(json!({"op": "create_session", "name": "", "cwd": project})).unwrap();
        assert_eq!(info["name"], "project");
        sid = info["id"].as_str().unwrap().to_string();

        // Plain message with an image.
        c.call(json!({"op": "send_message", "session": sid, "text": "hi",
            "images": [{"media_type": "image/png", "data": "AAAA"}]})).unwrap();
        assert_eq!(c.wait_event(|e| e["type"] == "info")["message"], "Model: fake-new");
        c.wait_event(|e| e["type"] == "text_delta");
        assert_eq!(c.wait_event(|e| e["type"] == "text")["text"], "echo:hi images:1");
        c.wait_event(|e| e["type"] == "turn_complete");

        // Tool use that needs permission.
        c.call(json!({"op": "send_message", "session": sid, "text": "run tool"})).unwrap();
        let req = c.wait_event(|e| e["type"] == "permission_request");
        assert_eq!(req["tool_name"], "Bash");
        let sessions = c.call(json!({"op": "list_sessions"})).unwrap();
        assert_eq!(sessions[0]["status"], "waiting_permission");
        c.call(json!({"op": "answer_permission", "session": sid, "request_id": req["request_id"], "allow": true})).unwrap();
        assert_eq!(c.wait_event(|e| e["type"] == "tool_result")["content"], "allow");
        c.wait_event(|e| e["type"] == "turn_complete");

        // History has everything except streaming deltas, in sequence order.
        let history = c.call(json!({"op": "get_history", "session": sid})).unwrap();
        let types: Vec<&str> = history.as_array().unwrap().iter().map(|e| e["event"]["type"].as_str().unwrap()).collect();
        assert!(!types.contains(&"text_delta"));
        assert_eq!(types.iter().filter(|t| **t == "user").count(), 2);
        let seqs: Vec<u64> = history.as_array().unwrap().iter().map(|e| e["seq"].as_u64().unwrap()).collect();
        assert!(seqs.windows(2).all(|w| w[0] < w[1]));

        // Files.
        let entries = c.call(json!({"op": "list_dir", "path": project})).unwrap();
        assert_eq!(entries[0]["name"], "a.txt");
        c.call(json!({"op": "write_file", "path": project.join("b.txt"), "content": "new"})).unwrap();
        assert_eq!(c.call(json!({"op": "read_file", "path": project.join("b.txt")})).unwrap()["content"], "new");
        assert!(c.call(json!({"op": "read_file", "path": project.join("missing")})).is_err());
    }

    // After a daemon restart the session is still there and Claude is resumed.
    let d = Daemon::start(&dir);
    let mut c = Client::connect(&d);
    let sessions = c.call(json!({"op": "list_sessions"})).unwrap();
    assert_eq!(sessions[0]["id"], sid.as_str());
    c.call(json!({"op": "send_message", "session": sid, "text": "again"})).unwrap();
    assert_eq!(c.wait_event(|e| e["type"] == "info" && e["message"] != json!("Session restored. Earlier messages aren't shown here, but Claude still has the conversation."))["message"], "Model: fake-resumed");
    assert_eq!(c.wait_event(|e| e["type"] == "text")["text"], "echo:again images:0");

    c.call(json!({"op": "close_session", "session": sid})).unwrap();
    assert_eq!(c.call(json!({"op": "list_sessions"})).unwrap(), json!([]));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn bridge_starts_daemon_and_relays() {
    use std::process::Stdio;
    let dir = tempdir("bridge");
    let socket = dir.join("sock/daemon.sock");
    let mut bridge = Command::new(env!("CARGO_BIN_EXE_codeviewd"))
        .arg("bridge")
        .env("CODEVIEW_SOCKET", &socket)
        .env("CODEVIEW_STATE_DIR", dir.join("state"))
        .env("CODEVIEW_CLAUDE_BIN", concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fake_claude.py"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdin = bridge.stdin.take().unwrap();
    let mut stdout = BufReader::new(bridge.stdout.take().unwrap());
    writeln!(stdin, "{}", json!({"id": 7, "request": {"op": "hello"}})).unwrap();
    let mut line = String::new();
    stdout.read_line(&mut line).unwrap();
    let resp: Value = serde_json::from_str(&line).unwrap();
    assert_eq!(resp["id"], 7);
    assert_eq!(resp["data"]["protocol"], 1);

    // Closing stdin ends the bridge, but the daemon it started keeps running.
    drop(stdin);
    assert!(bridge.wait().unwrap().success());
    assert!(UnixStream::connect(&socket).is_ok());

    // Shut the daemon down via its socket path owner: find it by the socket and SIGTERM it.
    let out = Command::new("fuser").arg(&socket).output();
    if let Ok(out) = out {
        for pid in String::from_utf8_lossy(&out.stdout).split_whitespace() {
            let _ = Command::new("kill").arg(pid).status();
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn git_status_diff_and_revert() {
    let dir = tempdir("git");
    let repo = dir.join("repo");
    std::fs::create_dir_all(&repo).unwrap();
    let git = |args: &[&str]| {
        let ok = Command::new("git").arg("-C").arg(&repo).args(args)
            .env("GIT_AUTHOR_NAME", "t").env("GIT_AUTHOR_EMAIL", "t@t")
            .env("GIT_COMMITTER_NAME", "t").env("GIT_COMMITTER_EMAIL", "t@t")
            .status().unwrap().success();
        assert!(ok, "git {args:?}");
    };
    git(&["init", "-q", "-b", "main"]);
    std::fs::write(repo.join("a.txt"), "one\n").unwrap();
    git(&["add", "a.txt"]);
    git(&["commit", "-q", "-m", "first"]);
    git(&["checkout", "-q", "-b", "feature"]);
    std::fs::write(repo.join("b.txt"), "branch file\n").unwrap();
    git(&["add", "b.txt"]);
    git(&["commit", "-q", "-m", "second"]);
    std::fs::write(repo.join("a.txt"), "one\ntwo\n").unwrap();
    std::fs::write(repo.join("new.txt"), "untracked\n").unwrap();

    let d = Daemon::start(&dir);
    let mut c = Client::connect(&d);
    let cwd = repo.to_str().unwrap();
    let st = c.call(json!({"op": "git_status", "cwd": cwd})).unwrap();
    assert_eq!(st["branch"], "feature");
    assert_eq!(st["base"], "main");
    let names = |key: &str| -> Vec<String> {
        let mut v: Vec<String> = st[key].as_array().unwrap().iter()
            .map(|f| format!("{}{}", f["status"].as_str().unwrap(), f["rel"].as_str().unwrap())).collect();
        v.sort();
        v
    };
    assert_eq!(names("uncommitted"), ["?new.txt", "Ma.txt"]);
    assert_eq!(names("branch_files"), ["Ab.txt", "Ma.txt"]);

    let diff = c.call(json!({"op": "git_diff", "cwd": cwd, "path": repo.join("a.txt")})).unwrap();
    assert!(diff["diff"].as_str().unwrap().contains("+two"), "{diff}");
    let diff = c.call(json!({"op": "git_diff", "cwd": cwd, "path": repo.join("b.txt"), "base": st["merge_base"]})).unwrap();
    assert!(diff["diff"].as_str().unwrap().contains("+branch file"), "{diff}");
    let diff = c.call(json!({"op": "git_diff", "cwd": cwd, "path": repo.join("new.txt")})).unwrap();
    assert!(diff["diff"].as_str().unwrap().contains("+untracked"), "{diff}");

    c.call(json!({"op": "git_revert", "cwd": cwd, "path": repo.join("a.txt")})).unwrap();
    c.call(json!({"op": "git_revert", "cwd": cwd, "path": repo.join("new.txt")})).unwrap();
    assert_eq!(std::fs::read_to_string(repo.join("a.txt")).unwrap(), "one\n");
    assert!(!repo.join("new.txt").exists());
    let st = c.call(json!({"op": "git_status", "cwd": cwd})).unwrap();
    assert!(st["uncommitted"].as_array().unwrap().is_empty(), "{st}");

    assert!(c.call(json!({"op": "git_status", "cwd": dir.to_str().unwrap()})).is_err());
}

#[test]
fn lsp_relay() {
    let dir = tempdir("lsp");
    std::fs::write(dir.join("main.rs"), "fn main() {}\n").unwrap();
    let file = dir.join("main.rs");
    let fake = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fake_lsp.py");
    let d = Daemon::start(&dir);
    let mut c = Client::connect(&d);

    let candidates = json!([["no-such-language-server"], [fake]]);
    let started = c.call(json!({"op": "lsp_start", "file": file, "candidates": candidates})).unwrap();
    assert_eq!(started["command"], json!([fake]));
    assert_eq!(started["existing"], false);
    let server = started["server"].as_u64().unwrap();

    let msg = json!({"jsonrpc": "2.0", "id": 7, "method": "initialize", "params": {}});
    c.call(json!({"op": "lsp_send", "server": server, "message": msg})).unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    let reply = loop {
        assert!(Instant::now() < deadline, "no reply from language server");
        let m = c.read();
        if m["kind"] == "event" && m["event"]["type"] == "lsp_message" && m["event"]["server"] == server {
            break m["event"]["message"].clone();
        }
    };
    assert_eq!(reply["id"], 7);
    assert_eq!(reply["result"]["echo"], "initialize");

    // Same client, root and command: the running server is reused.
    let again = c.call(json!({"op": "lsp_start", "file": file, "candidates": candidates})).unwrap();
    assert_eq!(again["server"], server);
    assert_eq!(again["existing"], true);

    let missing = c.call(json!({"op": "lsp_start", "file": file, "candidates": [["no-such-language-server"]]}));
    assert!(missing.unwrap_err().contains("no language server found"));
}

#[test]
fn list_and_resume_saved_conversation() {
    let dir = tempdir("resume");
    let project = dir.join("my.project");
    std::fs::create_dir_all(&project).unwrap();
    let project = project.canonicalize().unwrap();
    // A transcript as Claude Code writes it, under the folder name it derives from the cwd.
    let encoded: String = project.to_string_lossy().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '-' }).collect();
    let tdir = dir.join("claude/projects").join(encoded);
    std::fs::create_dir_all(&tdir).unwrap();
    let id = "0b1c2d3e-0000-4000-8000-000000000001";
    let lines = [
        json!({"type": "user", "isMeta": true, "message": {"role": "user", "content": "<local-command-caveat>x</local-command-caveat>"}}),
        json!({"type": "user", "message": {"role": "user", "content": "Fix   the\nbuild please"}}),
        json!({"type": "assistant", "message": {"content": [{"type": "text", "text": "On it."},
            {"type": "tool_use", "id": "t1", "name": "Bash", "input": {"command": "cargo build"}}]}}),
        json!({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t1", "content": "ok"}]}}),
        json!({"type": "assistant", "isSidechain": true, "message": {"content": [{"type": "text", "text": "subagent"}]}}),
        json!({"type": "user", "message": {"content": [{"type": "text", "text": "thanks"}]}}),
    ];
    std::fs::write(tdir.join(format!("{id}.jsonl")), lines.iter().map(|l| l.to_string() + "\n").collect::<String>()).unwrap();
    std::fs::write(tdir.join("0b1c2d3e-0000-4000-8000-000000000002.jsonl"), "").unwrap(); // no prompts: not listed

    let d = Daemon::start(&dir);
    let mut c = Client::connect(&d);
    let list = c.call(json!({"op": "list_conversations", "cwd": project})).unwrap();
    assert_eq!(list.as_array().unwrap().len(), 1, "{list}");
    assert_eq!(list[0]["id"], id);
    assert_eq!(list[0]["title"], "Fix the build please");
    assert_eq!(list[0]["prompts"], 2);
    assert_eq!(list[0]["recent"], true);
    assert!(list[0]["open_session"].is_null());

    let info = c.call(json!({"op": "create_session", "name": "", "cwd": project, "resume": id})).unwrap();
    assert_eq!(info["name"], "Fix the build please");
    let sid = info["id"].as_str().unwrap().to_string();
    let history = c.call(json!({"op": "get_history", "session": sid})).unwrap();
    let kinds: Vec<&str> = history.as_array().unwrap().iter().map(|e| e["event"]["type"].as_str().unwrap()).collect();
    assert_eq!(kinds, ["user", "text", "tool_use", "tool_result", "user", "info"]);
    assert_eq!(history[0]["event"]["text"], "Fix   the\nbuild please");

    // Listed as open now, and can't be opened twice.
    let list = c.call(json!({"op": "list_conversations", "cwd": project})).unwrap();
    assert_eq!(list[0]["open_session"], sid.as_str());
    assert!(c.call(json!({"op": "create_session", "name": "", "cwd": project, "resume": id})).is_err());
    assert!(c.call(json!({"op": "create_session", "name": "", "cwd": project, "resume": "../x"})).is_err());

    // The next message continues the conversation with --resume.
    c.call(json!({"op": "send_message", "session": sid, "text": "more"})).unwrap();
    assert_eq!(c.wait_event(|e| e["type"] == "info" && e["message"].as_str().unwrap().starts_with("Model"))["message"], "Model: fake-resumed");
    c.wait_event(|e| e["type"] == "turn_complete");
}

#[test]
fn finds_programs_installed_after_start() {
    let dir = tempdir("late");
    let d = Daemon::start(&dir);
    // Installed after the daemon started, into a folder only a login shell's PATH has.
    std::fs::create_dir_all(dir.join("late")).unwrap();
    std::fs::copy(concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fake_lsp.py"), dir.join("late/late-ls")).unwrap();
    let mut c = Client::connect(&d);
    let found = c.call(json!({"op": "which", "names": ["sh", "late-ls", "no-such-tool"]})).unwrap();
    assert_eq!(found, json!(["sh", "late-ls"]));
    std::fs::write(dir.join("a.txt"), "x").unwrap();
    let started = c.call(json!({"op": "lsp_start", "file": dir.join("a.txt"), "candidates": [["late-ls"]]})).unwrap();
    assert_eq!(started["command"][0], dir.join("late/late-ls").to_string_lossy().as_ref());
}
