//! Unix-socket server. Only the owning user can reach the socket (dir 0700,
//! socket 0600); remote access will go through `ssh host codeviewd bridge`.

use crate::daemon::Daemon;
use anyhow::{Context, Result, bail};
use codeview_protocol::{ClientMsg, ServerMsg, socket_path};
use std::os::unix::fs::PermissionsExt;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};
use tokio::signal::unix::{SignalKind, signal};
use tokio::sync::{broadcast, mpsc};

pub async fn serve() -> Result<()> {
    let path = socket_path();
    let dir = path.parent().context("socket path has no parent")?;
    std::fs::create_dir_all(dir)?;
    std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700))?;
    if path.exists() {
        if UnixStream::connect(&path).await.is_ok() {
            bail!("codeviewd is already running at {}", path.display());
        }
        std::fs::remove_file(&path)?;
    }
    let listener = UnixListener::bind(&path).with_context(|| format!("binding {}", path.display()))?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;

    let daemon = Daemon::load()?;
    eprintln!("codeviewd listening on {}", path.display());

    let mut term = signal(SignalKind::terminate())?;
    let mut int = signal(SignalKind::interrupt())?;
    loop {
        tokio::select! {
            accepted = listener.accept() => {
                let (stream, _) = accepted?;
                let d = daemon.clone();
                tokio::spawn(async move {
                    if let Err(e) = handle_client(d, stream).await {
                        eprintln!("client error: {e:#}");
                    }
                });
            }
            _ = term.recv() => break,
            _ = int.recv() => break,
        }
    }
    let _ = std::fs::remove_file(&path);
    Ok(())
}

async fn handle_client(daemon: Arc<Daemon>, stream: UnixStream) -> Result<()> {
    static NEXT_CLIENT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let client = NEXT_CLIENT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let result = serve_client(&daemon, client, stream).await;
    daemon.client_gone(client);
    result
}

async fn serve_client(daemon: &Arc<Daemon>, client: u64, stream: UnixStream) -> Result<()> {
    let (reader, mut writer) = stream.into_split();
    let mut events = daemon.subscribe();
    let (out_tx, mut out_rx) = mpsc::unbounded_channel::<ServerMsg>();

    let write_task = tokio::spawn(async move {
        loop {
            let msg = tokio::select! {
                m = out_rx.recv() => match m {
                    Some(m) => m,
                    None => break, // client finished; all replies written
                },
                ev = events.recv() => match ev {
                    Ok(m) => m,
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        eprintln!("client lagged; dropped {n} events");
                        continue;
                    }
                    Err(broadcast::error::RecvError::Closed) => break,
                },
            };
            let mut line = serde_json::to_string(&msg).expect("serializable");
            line.push('\n');
            if writer.write_all(line.as_bytes()).await.is_err() {
                break;
            }
        }
    });

    let mut lines = BufReader::new(reader).lines();
    while let Some(line) = lines.next_line().await? {
        if line.trim().is_empty() {
            continue;
        }
        let msg: ClientMsg = match serde_json::from_str(&line) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("bad request: {e}");
                continue;
            }
        };
        // Slow requests (git, auth checks) run on their own so they don't hold up the rest;
        // everything else stays in order.
        if msg.request.is_slow() {
            let (daemon, out_tx) = (daemon.clone(), out_tx.clone());
            tokio::spawn(async move {
                let _ = out_tx.send(respond(msg.id, daemon.handle(client, msg.request).await));
            });
        } else if out_tx.send(respond(msg.id, daemon.handle(client, msg.request).await)).is_err() {
            break;
        }
    }
    drop(out_tx);
    let _ = write_task.await;
    Ok(())
}

fn respond(id: u64, result: anyhow::Result<serde_json::Value>) -> ServerMsg {
    match result {
        Ok(data) => ServerMsg::Response { id, ok: true, data, error: None },
        Err(e) => ServerMsg::Response { id, ok: false, data: serde_json::Value::Null, error: Some(format!("{e:#}")) },
    }
}
