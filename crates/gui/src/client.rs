//! Connection to codeviewd over its Unix socket, starting the daemon if needed.

use crate::ffi;
use codeview_protocol::{ClientMsg, socket_path, state_dir};
use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::sync::mpsc;

static OUTGOING: OnceLock<mpsc::UnboundedSender<String>> = OnceLock::new();

pub fn start() {
    let (tx, rx) = mpsc::unbounded_channel();
    if OUTGOING.set(tx).is_err() {
        return;
    }
    std::thread::Builder::new()
        .name("daemon-client".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
            rt.block_on(run(rx));
        })
        .unwrap();
}

pub fn send(json: &str) {
    if let Err(e) = serde_json::from_str::<ClientMsg>(json) {
        eprintln!("codeview: dropping invalid request ({e}): {}", &json[..json.len().min(300)]);
        return;
    }
    if let Some(tx) = OUTGOING.get() {
        let _ = tx.send(json.to_string());
    }
}

async fn run(mut rx: mpsc::UnboundedReceiver<String>) {
    let mut spawned = false;
    loop {
        let stream = match connect(&mut spawned).await {
            Ok(s) => s,
            Err(e) => {
                ffi::deliver_connection_state(false, &e);
                tokio::time::sleep(Duration::from_secs(2)).await;
                continue;
            }
        };
        spawned = false;
        // Drop anything queued while disconnected; the GUI already failed those requests.
        while rx.try_recv().is_ok() {}
        ffi::deliver_connection_state(true, "");

        let (reader, mut writer) = stream.into_split();
        let mut lines = BufReader::new(reader).lines();
        loop {
            tokio::select! {
                line = lines.next_line() => match line {
                    Ok(Some(l)) => ffi::deliver_message(&l),
                    _ => break,
                },
                msg = rx.recv() => match msg {
                    Some(mut m) => {
                        m.push('\n');
                        if writer.write_all(m.as_bytes()).await.is_err() {
                            break;
                        }
                    }
                    None => return,
                },
            }
        }
        ffi::deliver_connection_state(false, "Lost connection to codeviewd");
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

async fn connect(spawned: &mut bool) -> Result<UnixStream, String> {
    let path = socket_path();
    if let Ok(s) = UnixStream::connect(&path).await {
        return Ok(s);
    }
    if !*spawned {
        *spawned = true;
        spawn_daemon()?;
    }
    for _ in 0..50 {
        tokio::time::sleep(Duration::from_millis(100)).await;
        if let Ok(s) = UnixStream::connect(&path).await {
            return Ok(s);
        }
    }
    Err(format!(
        "Can't reach codeviewd at {} (log: {})",
        path.display(),
        state_dir().join("daemon.log").display()
    ))
}

/// Starts codeviewd detached from this process, logging to the state dir.
fn spawn_daemon() -> Result<(), String> {
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
        .stdin(std::process::Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log)
        .process_group(0)
        .spawn()
        .map_err(|e| format!("Failed to start {}: {e}", bin.display()))?;
    // Reap it if it exits while we're running.
    std::thread::spawn(move || child.wait());
    Ok(())
}
