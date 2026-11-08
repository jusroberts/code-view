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
