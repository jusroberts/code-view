//! Session state shared by all connected clients.

use crate::claude::{self, Action, ClaudeProc};
use crate::{files, git, login, lsp, transcripts};
use anyhow::{Context, Result, anyhow, bail};
use codeview_protocol::*;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::broadcast;

pub struct Daemon {
    state: Mutex<State>,
    events: broadcast::Sender<ServerMsg>,
    state_file: PathBuf,
    pub claude_bin: String,
    lsp: Mutex<LspState>,
}

#[derive(Default)]
struct LspState {
    servers: HashMap<u64, lsp::LspServer>,
    next_id: u64,
}

#[derive(Default)]
struct State {
    sessions: HashMap<String, Session>,
    login: Option<login::LoginHandle>,
    next_gen: u64,
}

struct Session {
    info: SessionInfo,
    claude_session_id: String,
    /// Claude has created its transcript, so later spawns use `--resume`.
    started: bool,
    busy: bool,
    /// Set by an interrupt so the resulting failed turn reads as "Interrupted".
    interrupted: bool,
    history: Vec<SeqEvent>,
    next_seq: u64,
    proc: Option<ClaudeProc>,
    /// Pending permission requests: request_id -> original tool input.
    pending: HashMap<String, Value>,
}

/// What gets written to `sessions.json`.
#[derive(Serialize, Deserialize)]
struct StoredSession {
    info: SessionInfo,
    claude_session_id: String,
    started: bool,
}

impl Session {
    fn status(&self) -> SessionStatus {
        if !self.pending.is_empty() {
            SessionStatus::WaitingPermission
        } else if self.busy {
            SessionStatus::Busy
        } else {
            SessionStatus::Idle
        }
    }
}

impl Daemon {
    pub fn load() -> Result<Arc<Self>> {
        let dir = state_dir();
        std::fs::create_dir_all(&dir)?;
        let state_file = dir.join("sessions.json");
        let (events, _) = broadcast::channel(16384);
        let daemon = Arc::new(Daemon {
            state: Mutex::new(State::default()),
            events,
            state_file,
            claude_bin: std::env::var("CODEVIEW_CLAUDE_BIN").unwrap_or_else(|_| "claude".into()),
            lsp: Mutex::new(LspState::default()),
        });
        if let Ok(text) = std::fs::read_to_string(&daemon.state_file) {
            let stored: Vec<StoredSession> = serde_json::from_str(&text).context("parsing sessions.json")?;
            let mut st = daemon.state.lock().unwrap();
            for s in stored {
                let mut session = Session {
                    info: SessionInfo { status: SessionStatus::Idle, ..s.info },
                    claude_session_id: s.claude_session_id,
                    started: s.started,
                    busy: false,
                    interrupted: false,
                    history: Vec::new(),
                    next_seq: 0,
                    proc: None,
                    pending: HashMap::new(),
                };
                if session.started {
                    daemon.push(&mut session, SessionEvent::Info {
                        message: "Session restored. Earlier messages aren't shown here, but Claude still has the conversation.".into(),
                    });
                }
                st.sessions.insert(session.info.id.clone(), session);
            }
        }
        Ok(daemon)
    }

    pub fn subscribe(&self) -> broadcast::Receiver<ServerMsg> {
        self.events.subscribe()
    }

    pub fn broadcast(&self, event: Event) {
        let _ = self.events.send(ServerMsg::Event { event });
    }

    /// Records an event in the session's history and sends it to clients.
    fn push(&self, s: &mut Session, event: SessionEvent) {
        let seq = s.next_seq;
        s.next_seq += 1;
        let se = SeqEvent { seq, event };
        if !se.event.is_transient() {
            s.history.push(se.clone());
        }
        self.broadcast(Event::Session { session: s.info.id.clone(), event: se });
    }

    fn session_list(st: &State) -> Vec<SessionInfo> {
        let mut v: Vec<SessionInfo> = st
            .sessions
            .values()
            .map(|s| SessionInfo { status: s.status(), ..s.info.clone() })
            .collect();
        v.sort_by(|a, b| a.created_at.cmp(&b.created_at).then_with(|| a.name.cmp(&b.name)));
        v
    }

    fn sessions_changed(&self, st: &State) {
        self.broadcast(Event::SessionsChanged { sessions: Self::session_list(st) });
    }

    fn persist(&self, st: &State) {
        let stored: Vec<StoredSession> = st
            .sessions
            .values()
            .map(|s| StoredSession {
                info: s.info.clone(),
                claude_session_id: s.claude_session_id.clone(),
                started: s.started,
            })
            .collect();
        let tmp = self.state_file.with_extension("json.tmp");
        let result = std::fs::write(&tmp, serde_json::to_vec_pretty(&stored).unwrap())
            .and_then(|_| std::fs::rename(&tmp, &self.state_file));
        if let Err(e) = result {
            eprintln!("failed to save sessions: {e}");
        }
    }

    /// Handles one request from client `client` (a per-connection id).
    pub async fn handle(self: &Arc<Self>, client: u64, req: Request) -> Result<Value> {
        match req {
            Request::Hello => Ok(json!({
                "protocol": PROTOCOL_VERSION,
                "version": env!("CARGO_PKG_VERSION"),
                "commit": env!("CODEVIEW_COMMIT"),
                "hostname": hostname(),
                "home": home_dir(),
            })),
            Request::ListSessions => Ok(json!(Self::session_list(&self.state.lock().unwrap()))),
            Request::CreateSession { name, cwd, permission_mode, resume } => self.create_session(name, cwd, permission_mode, resume),
            Request::ListConversations { cwd } => {
                let cwd = files::expand(&cwd).canonicalize()?.to_string_lossy().into_owned();
                let open: HashMap<String, String> = {
                    let st = self.state.lock().unwrap();
                    st.sessions.values().map(|s| (s.claude_session_id.clone(), s.info.id.clone())).collect()
                };
                Ok(json!(transcripts::list(&cwd, &|id| open.get(id).cloned())))
            }
            Request::RenameSession { session, name } => {
                let mut st = self.state.lock().unwrap();
                st.sessions.get_mut(&session).context("no such session")?.info.name = name;
                self.persist(&st);
                self.sessions_changed(&st);
                Ok(Value::Null)
            }
            Request::CloseSession { session } => {
                let mut st = self.state.lock().unwrap();
                // Dropping the session drops its ClaudeProc, which kills the process.
                st.sessions.remove(&session).context("no such session")?;
                self.persist(&st);
                self.sessions_changed(&st);
                Ok(Value::Null)
            }
            Request::GetHistory { session } => {
                let st = self.state.lock().unwrap();
                Ok(json!(st.sessions.get(&session).context("no such session")?.history))
            }
            Request::SendMessage { session, text, images } => self.send_message(&session, text, images),
            Request::Interrupt { session } => {
                let mut st = self.state.lock().unwrap();
                let s = st.sessions.get_mut(&session).context("no such session")?;
                if let Some(p) = &s.proc {
                    p.send(claude::interrupt_request());
                    s.interrupted = s.busy;
                }
                for request_id in std::mem::take(&mut s.pending).into_keys() {
                    self.push(s, SessionEvent::PermissionResolved { request_id, allowed: false });
                }
                self.sessions_changed(&st);
                Ok(Value::Null)
            }
            Request::AnswerPermission { session, request_id, allow } => {
                let mut st = self.state.lock().unwrap();
                let s = st.sessions.get_mut(&session).context("no such session")?;
                let input = s.pending.remove(&request_id).context("permission request is no longer pending")?;
                if let Some(p) = &s.proc {
                    p.send(claude::permission_response(&request_id, allow, input));
                }
                self.push(s, SessionEvent::PermissionResolved { request_id, allowed: allow });
                self.sessions_changed(&st);
                Ok(Value::Null)
            }
            Request::ListDir { path } => Ok(json!(files::list_dir(&path)?)),
            Request::ReadFile { path } => Ok(json!({ "content": files::read_file(&path)? })),
            Request::WriteFile { path, content } => {
                files::write_file(&path, &content)?;
                Ok(Value::Null)
            }
            Request::AuthStatus => self.auth_status().await,
            Request::GitStatus { cwd } => Ok(json!(git::status(&cwd).await?)),
            Request::GitDiff { cwd, path, base } => Ok(json!({ "diff": git::diff(&cwd, &path, base.as_deref()).await? })),
            Request::GitRevert { cwd, path } => {
                git::revert(&cwd, &path).await?;
                Ok(Value::Null)
            }
            Request::LspStart { file, candidates } => self.lsp_start(client, &file, &candidates).await,
            Request::LspSend { server, message } => {
                let lsp = self.lsp.lock().unwrap();
                let s = lsp.servers.get(&server).ok_or_else(|| anyhow!("language server {server} isn't running"))?;
                s.send(&message);
                Ok(Value::Null)
            }
            Request::LspStop { server } => {
                self.lsp.lock().unwrap().servers.remove(&server);
                Ok(Value::Null)
            }
            Request::StartLogin => {
                let mut st = self.state.lock().unwrap();
                if st.login.is_some() {
                    bail!("a login is already in progress");
                }
                st.login = Some(login::start(self.clone())?);
                Ok(Value::Null)
            }
            Request::LoginInput { text } => {
                let mut st = self.state.lock().unwrap();
                st.login.as_mut().context("no login in progress")?.input(&text)?;
                Ok(Value::Null)
            }
            Request::CancelLogin => {
                let login = self.state.lock().unwrap().login.take();
                if let Some(mut l) = login {
                    l.kill();
                }
                Ok(Value::Null)
            }
        }
    }

    fn create_session(&self, name: String, cwd: String, permission_mode: Option<String>, resume: Option<String>) -> Result<Value> {
        let cwd = files::expand(&cwd);
        if !cwd.is_dir() {
            bail!("{} is not a directory", cwd.display());
        }
        let cwd = cwd.canonicalize()?.to_string_lossy().into_owned();
        // Resuming: load the saved conversation so its earlier messages show in the chat.
        let (history, title) = match &resume {
            Some(claude_id) => {
                if let Some(s) = self.state.lock().unwrap().sessions.values().find(|s| &s.claude_session_id == claude_id) {
                    bail!("this conversation is already open as “{}”", s.info.name);
                }
                transcripts::history(&transcripts::transcript_path(&cwd, claude_id)?)
            }
            None => (Vec::new(), String::new()),
        };
        let id = uuid::Uuid::new_v4().to_string();
        let name = if !name.trim().is_empty() {
            name
        } else if !title.is_empty() {
            title.chars().take(40).collect()
        } else {
            default_name(&cwd)
        };
        let info = SessionInfo {
            id: id.clone(),
            name,
            cwd,
            backend: "claude".into(),
            permission_mode: permission_mode.filter(|m| !m.is_empty()),
            status: SessionStatus::Idle,
            created_at: SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs(),
        };
        let mut session = Session {
            info: info.clone(),
            claude_session_id: resume.clone().unwrap_or_else(|| uuid::Uuid::new_v4().to_string()),
            started: resume.is_some(),
            busy: false,
            interrupted: false,
            history: Vec::new(),
            next_seq: 0,
            proc: None,
            pending: HashMap::new(),
        };
        for event in history {
            self.push(&mut session, event);
        }
        if resume.is_some() {
            self.push(&mut session, SessionEvent::Info { message: "Resumed an earlier conversation.".into() });
        }
        let mut st = self.state.lock().unwrap();
        st.sessions.insert(id, session);
        self.persist(&st);
        self.sessions_changed(&st);
        Ok(json!(info))
    }

    fn send_message(self: &Arc<Self>, session: &str, text: String, images: Vec<ImageAttachment>) -> Result<Value> {
        if text.trim().is_empty() && images.is_empty() {
            bail!("message is empty");
        }
        let mut guard = self.state.lock().unwrap();
        let st = &mut *guard;
        let s = st.sessions.get_mut(session).context("no such session")?;
        if s.proc.is_none() {
            st.next_gen += 1;
            let p = claude::spawn(
                self.clone(),
                claude::SpawnArgs {
                    bin: self.claude_bin.clone(),
                    session: s.info.id.clone(),
                    generation: st.next_gen,
                    cwd: s.info.cwd.clone(),
                    claude_session_id: s.claude_session_id.clone(),
                    resume: s.started,
                    permission_mode: s.info.permission_mode.clone(),
                },
            )?;
            s.proc = Some(p);
        }
        s.proc.as_ref().unwrap().send(claude::user_message(&text, &images));
        s.busy = true;
        self.push(s, SessionEvent::User { text, images: images.len() });
        self.sessions_changed(st);
        Ok(Value::Null)
    }

    /// Called by the Claude reader task for every parsed stdout line.
    pub fn on_claude_actions(&self, session: &str, generation: u64, actions: Vec<Action>) {
        let mut st = self.state.lock().unwrap();
        let Some(s) = st.sessions.get_mut(session) else { return };
        if s.proc.as_ref().map(|p| p.generation) != Some(generation) {
            return;
        }
        let mut changed = false;
        let mut persist = false;
        for action in actions {
            match action {
                Action::Event(mut e) => {
                    if let SessionEvent::TurnComplete { is_error, result, .. } = &mut e {
                        if std::mem::take(&mut s.interrupted) && *is_error {
                            *result = Some("Interrupted.".into());
                        }
                        s.busy = false;
                        changed = true;
                    }
                    self.push(s, e);
                }
                Action::Init { session_id, model } => {
                    if !s.started || s.claude_session_id != session_id {
                        s.started = true;
                        s.claude_session_id = session_id;
                        persist = true;
                    }
                    if let Some(model) = model {
                        self.push(s, SessionEvent::Info { message: format!("Model: {model}") });
                    }
                }
                Action::Permission { request_id, tool_name, input, description } => {
                    s.pending.insert(request_id.clone(), input.clone());
                    self.push(s, SessionEvent::PermissionRequest { request_id, tool_name, input, description });
                    changed = true;
                }
                Action::Reply(line) => {
                    if let Some(p) = &s.proc {
                        p.send(line);
                    }
                }
            }
        }
        if persist {
            self.persist(&st);
        }
        if changed {
            self.sessions_changed(&st);
        }
    }

    /// Called when a Claude process exits.
    pub fn on_claude_exit(&self, session: &str, generation: u64, code: Option<i32>, stderr_tail: String) {
        let mut st = self.state.lock().unwrap();
        let Some(s) = st.sessions.get_mut(session) else { return };
        if s.proc.as_ref().map(|p| p.generation) != Some(generation) {
            return;
        }
        s.proc = None;
        for request_id in std::mem::take(&mut s.pending).into_keys() {
            self.push(s, SessionEvent::PermissionResolved { request_id, allowed: false });
        }
        if s.busy || code != Some(0) {
            let detail = stderr_tail.trim();
            let message = match code {
                Some(c) => format!("Claude exited with code {c}"),
                None => "Claude was killed".to_string(),
            };
            let message = if detail.is_empty() { message } else { format!("{message}:\n{detail}") };
            self.push(s, SessionEvent::Error { message });
        }
        s.busy = false;
        self.sessions_changed(&st);
    }

    async fn auth_status(&self) -> Result<Value> {
        let mut cmd = tokio::process::Command::new(&self.claude_bin);
        cmd.args(["auth", "status", "--json"]).stdin(std::process::Stdio::null());
        claude::scrub_env(&mut cmd);
        let out = tokio::time::timeout(Duration::from_secs(30), cmd.output())
            .await
            .map_err(|_| anyhow!("`claude auth status` timed out"))?
            .with_context(|| format!("running {}", self.claude_bin))?;
        // `auth status` exits non-zero when logged out but still prints JSON.
        serde_json::from_slice(&out.stdout).or_else(|_| {
            Ok(json!({
                "loggedIn": false,
                "error": String::from_utf8_lossy(&out.stderr).trim().to_string(),
            }))
        })
    }

    pub fn login_output(&self, text: String) {
        self.broadcast(Event::LoginOutput { text });
    }

    pub fn login_finished(&self, success: bool) {
        self.state.lock().unwrap().login = None;
        self.broadcast(Event::LoginFinished { success });
    }

    async fn lsp_start(self: &Arc<Self>, client: u64, file: &str, candidates: &[Vec<String>]) -> Result<Value> {
        let command = lsp::pick_command(candidates)?;
        let root = lsp::root_for(file).await.to_string_lossy().into_owned();
        let mut lsp = self.lsp.lock().unwrap();
        if let Some((id, _)) = lsp.servers.iter().find(|(_, s)| s.owner == client && s.root == root && s.command == command) {
            return Ok(json!({"server": id, "root": root, "command": command, "existing": true}));
        }
        lsp.next_id += 1;
        let id = lsp.next_id;
        let server = lsp::spawn(self.clone(), id, client, root.clone(), command.clone())?;
        lsp.servers.insert(id, server);
        Ok(json!({"server": id, "root": root, "command": command, "existing": false}))
    }

    pub fn lsp_exited(&self, server: u64, error: Option<String>) {
        if self.lsp.lock().unwrap().servers.remove(&server).is_some() || error.is_some() {
            self.broadcast(Event::LspExited { server, error });
        }
    }

    /// A client disconnected: stop the language servers it started.
    pub fn client_gone(&self, client: u64) {
        self.lsp.lock().unwrap().servers.retain(|_, s| s.owner != client);
    }
}

fn default_name(cwd: &str) -> String {
    std::path::Path::new(cwd)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| cwd.to_string())
}

fn hostname() -> String {
    std::fs::read_to_string("/proc/sys/kernel/hostname")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "localhost".into())
}
