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
        let child = Command::new(env!("CARGO_BIN_EXE_codeviewd"))
            .env("CODEVIEW_SOCKET", &socket)
            .env("CODEVIEW_STATE_DIR", dir.join("state"))
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
