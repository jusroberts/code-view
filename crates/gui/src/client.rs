//! Connections to codeviewd daemons: the local one over its Unix socket
//! (starting it if needed), remote ones via `ssh <host> codeviewd bridge`.

use crate::ffi;
use codeview_protocol::{ClientMsg, socket_path, state_dir};
use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, BufReader, Lines};
use tokio::net::UnixStream;
use tokio::process::{Child, Command};
use tokio::sync::mpsc;
use tokio::task::{AbortHandle, JoinHandle};

/// Remote command run by ssh. A login shell picks up PATH entries like ~/.cargo/bin.
const REMOTE_COMMAND: &str = r#"exec "$SHELL" -lc 'exec codeviewd bridge'"#;

struct Conn {
    tx: mpsc::UnboundedSender<String>,
    task: AbortHandle,
}

fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("daemon-client")
            .enable_all()
            .build()
            .unwrap()
    })
}

fn conns() -> &'static Mutex<HashMap<String, Conn>> {
    static CONNS: OnceLock<Mutex<HashMap<String, Conn>>> = OnceLock::new();
    CONNS.get_or_init(Default::default)
}

pub fn open(id: &str, ssh_target: &str) {
    close(id);
    let (tx, rx) = mpsc::unbounded_channel();
    let target = (!ssh_target.is_empty()).then(|| ssh_target.to_string());
    let task = runtime().spawn(run(id.to_string(), target, rx)).abort_handle();
    conns().lock().unwrap().insert(id.to_string(), Conn { tx, task });
}

pub fn close(id: &str) {
    // Aborting drops the ssh child, which kill_on_drop terminates.
    if let Some(c) = conns().lock().unwrap().remove(id) {
        c.task.abort();
    }
}

pub fn send(id: &str, json: &str) {
    if let Err(e) = serde_json::from_str::<ClientMsg>(json) {
        eprintln!("codeview: dropping invalid request ({e}): {}", &json[..json.len().min(300)]);
        return;
    }
    if let Some(c) = conns().lock().unwrap().get(id) {
        let _ = c.tx.send(json.to_string());
    }
}

struct Transport {
    lines: Lines<BufReader<Box<dyn AsyncRead + Send + Unpin>>>,
    writer: Box<dyn AsyncWrite + Send + Unpin>,
    /// For ssh: the child process and a task collecting its stderr.
    ssh: Option<(Child, JoinHandle<String>)>,
}

impl Transport {
    /// Why the connection ended, for display.
    async fn close_reason(self) -> String {
        drop(self.writer);
        match self.ssh {
            None => "Lost connection to codeviewd".into(),
            Some((mut child, stderr)) => {
                let _ = tokio::time::timeout(Duration::from_secs(2), child.wait()).await;
                let _ = child.kill().await;
                let err = stderr.await.unwrap_or_default();
                let err = err.trim();
                if err.is_empty() { "Connection closed".into() } else { err.to_string() }
            }
        }
    }
}

async fn run(id: String, target: Option<String>, mut rx: mpsc::UnboundedReceiver<String>) {
    let mut spawned = false;
    let mut backoff = Duration::from_secs(1);
    loop {
        match connect(target.as_deref(), &mut spawned).await {
            Ok(mut t) => {
                spawned = false;
                backoff = Duration::from_secs(1);
                // Drop anything queued while disconnected; the GUI already failed those requests.
                while rx.try_recv().is_ok() {}
                ffi::deliver_connection_state(&id, true, "");
                let ended = loop {
                    tokio::select! {
                        line = t.lines.next_line() => match line {
                            Ok(Some(l)) => ffi::deliver_message(&id, &l),
                            _ => break true,
                        },
                        msg = rx.recv() => match msg {
                            Some(mut m) => {
                                m.push('\n');
                                if t.writer.write_all(m.as_bytes()).await.is_err() || t.writer.flush().await.is_err() {
                                    break true;
                                }
                            }
                            None => break false,
                        },
                    }
                };
                if !ended {
                    return;
                }
                let reason = t.close_reason().await;
                ffi::deliver_connection_state(&id, false, &reason);
            }
            Err(e) => ffi::deliver_connection_state(&id, false, &e),
        }
        tokio::time::sleep(backoff).await;
        if target.is_some() {
            backoff = (backoff * 2).min(Duration::from_secs(30));
        }
    }
}

async fn connect(target: Option<&str>, spawned: &mut bool) -> Result<Transport, String> {
    let mut t = match target {
        None => connect_local(spawned).await?,
        Some(host) => connect_ssh(host)?,
    };
    // Don't report "connected" until a daemon actually answers.
    let probe = r#"{"id":0,"request":{"op":"hello"}}"#;
    let handshake = async {
        t.writer.write_all(format!("{probe}\n").as_bytes()).await.ok()?;
        t.writer.flush().await.ok()?;
        loop {
            let line = t.lines.next_line().await.ok()??;
            let v: serde_json::Value = serde_json::from_str(&line).ok()?;
            if v["kind"] == "response" && v["id"] == 0 {
                return Some(());
            }
        }
    };
    match tokio::time::timeout(Duration::from_secs(30), handshake).await {
        Ok(Some(())) => Ok(t),
        Ok(None) => Err(t.close_reason().await),
        Err(_) => {
            let _ = t.close_reason().await;
            Err("Timed out waiting for codeviewd".into())
        }
    }
}

async fn connect_local(spawned: &mut bool) -> Result<Transport, String> {
    let path = socket_path();
    let stream = match UnixStream::connect(&path).await {
        Ok(s) => s,
        Err(_) => {
            if !*spawned {
                *spawned = true;
                spawn_local_daemon()?;
            }
            let mut found = None;
            for _ in 0..50 {
                tokio::time::sleep(Duration::from_millis(100)).await;
                if let Ok(s) = UnixStream::connect(&path).await {
                    found = Some(s);
                    break;
                }
            }
            found.ok_or_else(|| {
                format!(
                    "Can't reach codeviewd at {} (log: {})",
                    path.display(),
                    state_dir().join("daemon.log").display()
                )
            })?
        }
    };
    let (r, w) = stream.into_split();
    Ok(Transport { lines: BufReader::new(Box::new(r) as Box<dyn AsyncRead + Send + Unpin>).lines(), writer: Box::new(w), ssh: None })
}

/// `ssh -T <host> codeviewd bridge`. BatchMode means keys/agent only: the GUI
/// has no terminal to answer password or host-key prompts.
fn connect_ssh(host: &str) -> Result<Transport, String> {
    let ssh = std::env::var("CODEVIEW_SSH").unwrap_or_else(|_| "ssh".into());
    let mut child = Command::new(&ssh)
        .args([
            "-T",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=15",
            "-o", "ServerAliveCountMax=3",
            host,
            REMOTE_COMMAND,
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .map_err(|e| format!("Failed to run {ssh}: {e}"))?;
    let stdout = child.stdout.take().unwrap();
    let stdin = child.stdin.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let stderr_task = tokio::spawn(async move {
        let mut buf = Vec::new();
        let _ = stderr.read_to_end(&mut buf).await;
        let text = String::from_utf8_lossy(&buf).into_owned();
        let start = text.floor_char_boundary(text.len().saturating_sub(1000));
        text[start..].to_string()
    });
    Ok(Transport {
        lines: BufReader::new(Box::new(stdout) as Box<dyn AsyncRead + Send + Unpin>).lines(),
        writer: Box::new(stdin),
        ssh: Some((child, stderr_task)),
    })
}

/// Starts the local codeviewd detached from this process, logging to the state dir.
fn spawn_local_daemon() -> Result<(), String> {
    use std::os::unix::process::CommandExt;
    let sibling = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join("codeviewd")));
    let bin: PathBuf = sibling.filter(|p| p.exists()).unwrap_or_else(|| "codeviewd".into());
    let dir = state_dir();
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join("daemon.log"))
        .map_err(|e| e.to_string())?;
    let mut child = std::process::Command::new(&bin)
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log)
        .process_group(0)
        .spawn()
        .map_err(|e| format!("Failed to start {}: {e}", bin.display()))?;
    // Reap it if it exits while we're running.
    std::thread::spawn(move || child.wait());
    Ok(())
}
