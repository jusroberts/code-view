//! `codeviewd bridge`: relays stdin/stdout to the local daemon socket, starting
//! the daemon first if needed. The GUI runs `ssh host codeviewd bridge`, so
//! remote access is authenticated and encrypted by SSH and no port is opened.

use anyhow::{Context, Result};
use codeview_protocol::{socket_path, state_dir};
use std::os::unix::process::CommandExt;
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::net::UnixStream;

pub async fn bridge() -> Result<()> {
    let stream = connect_or_spawn().await?;
    let (mut sock_r, mut sock_w) = stream.into_split();
    let mut stdin = tokio::io::stdin();
    let mut stdout = tokio::io::stdout();
    let to_daemon = async {
        let _ = tokio::io::copy(&mut stdin, &mut sock_w).await;
        // Half-close so the daemon finishes replying, then closes its side.
        let _ = sock_w.shutdown().await;
        std::future::pending::<()>().await;
    };
    tokio::select! {
        _ = to_daemon => {}
        _ = tokio::io::copy(&mut sock_r, &mut stdout) => {}
    }
    let _ = stdout.flush().await;
    // Exit now: tokio's blocking stdin reader would otherwise keep the process alive.
    std::process::exit(0);
}

async fn connect_or_spawn() -> Result<UnixStream> {
    let path = socket_path();
    if let Ok(s) = UnixStream::connect(&path).await {
        return Ok(s);
    }
    spawn_daemon()?;
    for _ in 0..50 {
        tokio::time::sleep(Duration::from_millis(100)).await;
        if let Ok(s) = UnixStream::connect(&path).await {
            return Ok(s);
        }
    }
    anyhow::bail!("codeviewd didn't start; see {}", state_dir().join("daemon.log").display())
}

/// Starts `codeviewd serve` in its own session so it outlives the SSH connection.
fn spawn_daemon() -> Result<()> {
    let dir = state_dir();
    std::fs::create_dir_all(&dir)?;
    let log = std::fs::OpenOptions::new().create(true).append(true).open(dir.join("daemon.log"))?;
    let mut cmd = std::process::Command::new(std::env::current_exe()?);
    cmd.arg("serve")
        .stdin(std::process::Stdio::null())
        .stdout(log.try_clone()?)
        .stderr(log);
    // SAFETY: setsid is async-signal-safe and touches no Rust state.
    unsafe {
        cmd.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    cmd.spawn().context("starting codeviewd serve")?;
    Ok(())
}
