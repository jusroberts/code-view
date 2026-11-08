//! Runs `claude auth login` in a pseudo-terminal so the GUI can show the login
//! URL and type the pasted auth code back in. Works the same on remote hosts,
//! where Claude can't open a browser itself.

use crate::daemon::Daemon;
use anyhow::{Context, Result};
use portable_pty::{ChildKiller, CommandBuilder, MasterPty, PtySize, native_pty_system};
use std::io::{Read, Write};
use std::sync::Arc;

pub struct LoginHandle {
    writer: Box<dyn Write + Send>,
    killer: Box<dyn ChildKiller + Send + Sync>,
    _master: Box<dyn MasterPty + Send>,
}

impl LoginHandle {
    pub fn input(&mut self, text: &str) -> Result<()> {
        self.writer.write_all(text.as_bytes())?;
        self.writer.write_all(b"\r")?;
        self.writer.flush()?;
        Ok(())
    }

    pub fn kill(&mut self) {
        let _ = self.killer.kill();
    }
}

pub fn start(daemon: Arc<Daemon>) -> Result<LoginHandle> {
    let pair = native_pty_system()
        // Very wide so the login URL is never wrapped across lines.
        .openpty(PtySize { rows: 40, cols: 1000, pixel_width: 0, pixel_height: 0 })
        .context("opening a pseudo-terminal")?;
    let mut cmd = CommandBuilder::new(&daemon.claude_bin);
    cmd.args(["auth", "login"]);
    cmd.env_remove("CLAUDECODE");
    cmd.env_remove("CLAUDE_CODE_ENTRYPOINT");
    cmd.cwd(codeview_protocol::home_dir());
    let mut child = pair.slave.spawn_command(cmd).context("starting claude auth login")?;
    drop(pair.slave);

    let mut reader = pair.master.try_clone_reader()?;
    let writer = pair.master.take_writer()?;
    let killer = child.clone_killer();

    let d = daemon.clone();
    std::thread::spawn(move || {
        let mut buf = [0u8; 4096];
        while let Ok(n) = reader.read(&mut buf) {
            if n == 0 {
                break;
            }
            let clean = strip_ansi_escapes::strip(&buf[..n]);
            let text = String::from_utf8_lossy(&clean).replace("\r\n", "\n").replace('\r', "\n");
            if !text.trim().is_empty() {
                d.login_output(text);
            }
        }
    });
    std::thread::spawn(move || {
        let success = child.wait().map(|s| s.success()).unwrap_or(false);
        daemon.login_finished(success);
    });

    Ok(LoginHandle { writer, killer, _master: pair.master })
}
