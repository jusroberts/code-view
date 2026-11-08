//! Language servers run next to the files (so remote hosts work too). The daemon only
//! spawns them and relays JSON-RPC; the GUI is the LSP client.

use crate::daemon::Daemon;
use crate::files::expand;
use anyhow::{Context, Result, bail};
use codeview_protocol::Event;
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;
use tokio::sync::{mpsc, oneshot};

/// Handle to a running server. Dropping it kills the process.
pub struct LspServer {
    pub owner: u64,
    pub root: String,
    pub command: Vec<String>,
    stdin: mpsc::UnboundedSender<Vec<u8>>,
    _kill: oneshot::Sender<()>,
}

impl LspServer {
    pub fn send(&self, message: &Value) {
        let body = message.to_string();
        let mut frame = format!("Content-Length: {}\r\n\r\n", body.len()).into_bytes();
        frame.extend_from_slice(body.as_bytes());
        let _ = self.stdin.send(frame);
    }
}

fn on_path(program: &str) -> bool {
    if program.contains('/') {
        return Path::new(program).is_file();
    }
    std::env::var_os("PATH")
        .map(|paths| std::env::split_paths(&paths).any(|dir| dir.join(program).is_file()))
        .unwrap_or(false)
}

pub fn pick_command(candidates: &[Vec<String>]) -> Result<Vec<String>> {
    for c in candidates {
        if c.first().is_some_and(|p| on_path(p)) {
            return Ok(c.clone());
        }
    }
    let names: Vec<&str> = candidates.iter().filter_map(|c| c.first().map(String::as_str)).collect();
    bail!("no language server found on this host (looked for {})", names.join(", "))
}

/// The file's git repository root, else its folder.
pub async fn root_for(file: &str) -> PathBuf {
    let path = expand(file);
    let dir = path.parent().map(Path::to_path_buf).unwrap_or(path);
    let out = Command::new("git").arg("-C").arg(&dir).args(["rev-parse", "--show-toplevel"]).output().await;
    match out {
        Ok(o) if o.status.success() => PathBuf::from(String::from_utf8_lossy(&o.stdout).trim()),
        _ => dir,
    }
}

pub fn spawn(daemon: Arc<Daemon>, id: u64, owner: u64, root: String, command: Vec<String>) -> Result<LspServer> {
    let mut child = Command::new(&command[0])
        .args(&command[1..])
        .current_dir(&root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .with_context(|| format!("starting {}", command[0]))?;
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let (tx, mut rx) = mpsc::unbounded_channel::<Vec<u8>>();
    let (kill_tx, kill_rx) = oneshot::channel::<()>();

    tokio::spawn(async move {
        while let Some(frame) = rx.recv().await {
            if stdin.write_all(&frame).await.is_err() || stdin.flush().await.is_err() {
                break;
            }
        }
    });

    let d = daemon.clone();
    let reader = tokio::spawn(async move {
        let mut r = BufReader::new(stdout);
        while let Some(message) = read_message(&mut r).await {
            d.broadcast(Event::LspMessage { server: id, message });
        }
    });

    let stderr_task = tokio::spawn(async move {
        // Keep only the tail; servers can be chatty.
        let mut tail = Vec::new();
        let mut buf = [0u8; 4096];
        while let Ok(n) = stderr.read(&mut buf).await {
            if n == 0 {
                break;
            }
            tail.extend_from_slice(&buf[..n]);
            if tail.len() > 8192 {
                tail.drain(..tail.len() - 4096);
            }
        }
        String::from_utf8_lossy(&tail).trim().to_string()
    });

    tokio::spawn(async move {
        let status = tokio::select! {
            s = child.wait() => s.ok(),
            _ = kill_rx => { let _ = child.kill().await; None }
        };
        let _ = reader.await;
        let tail = stderr_task.await.unwrap_or_default();
        let error = match status {
            Some(s) if !s.success() => Some(format!("exited with {s}{}{}", if tail.is_empty() { "" } else { ": " }, last_lines(&tail, 5))),
            _ => None,
        };
        daemon.lsp_exited(id, error);
    });

    Ok(LspServer { owner, root, command, stdin: tx, _kill: kill_tx })
}

fn last_lines(s: &str, n: usize) -> String {
    let lines: Vec<&str> = s.lines().collect();
    lines[lines.len().saturating_sub(n)..].join("\n")
}

/// Reads one `Content-Length`-framed JSON-RPC message.
async fn read_message<R: tokio::io::AsyncBufRead + Unpin>(r: &mut R) -> Option<Value> {
    loop {
        let mut len = None;
        loop {
            let mut line = String::new();
            if r.read_line(&mut line).await.ok()? == 0 {
                return None;
            }
            let line = line.trim_end();
            if line.is_empty() {
                break;
            }
            if let Some(v) = line.strip_prefix("Content-Length:") {
                len = v.trim().parse::<usize>().ok();
            }
        }
        let Some(len) = len else { continue };
        let mut body = vec![0u8; len];
        r.read_exact(&mut body).await.ok()?;
        if let Ok(v) = serde_json::from_slice(&body) {
            return Some(v);
        }
    }
}
