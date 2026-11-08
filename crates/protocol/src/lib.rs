//! Wire protocol between the `codeview` GUI and the `codeviewd` daemon.
//!
//! Framing: one JSON object per line (LF-terminated) in each direction.
//! The client sends [`ClientMsg`]; the daemon sends [`ServerMsg`].

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::PathBuf;

pub const PROTOCOL_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClientMsg {
    pub id: u64,
    pub request: Request,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum Request {
    /// Returns `{protocol, version, hostname, home}`.
    Hello,
    /// Returns `[SessionInfo]`.
    ListSessions,
    /// Returns `SessionInfo`. `permission_mode: None` uses the user's Claude settings.
    CreateSession {
        name: String,
        cwd: String,
        #[serde(default)]
        permission_mode: Option<String>,
    },
    RenameSession { session: String, name: String },
    CloseSession { session: String },
    /// Returns `[SeqEvent]`.
    GetHistory { session: String },
    SendMessage {
        session: String,
        text: String,
        #[serde(default)]
        images: Vec<ImageAttachment>,
    },
    Interrupt { session: String },
    AnswerPermission {
        session: String,
        request_id: String,
        allow: bool,
    },
    /// Returns `[DirEntry]`.
    ListDir { path: String },
    /// Returns `{content}`.
    ReadFile { path: String },
    WriteFile { path: String, content: String },
    /// Returns the JSON from `claude auth status --json`.
    AuthStatus,
    /// Starts `claude auth login` in a pseudo-terminal; output arrives as `Event::LoginOutput`.
    StartLogin,
    /// Types a line (e.g. a pasted auth code) into the login terminal.
    LoginInput { text: String },
    CancelLogin,
    /// Returns `GitStatus` for the repository containing `cwd`.
    GitStatus { cwd: String },
    /// Returns `{diff}`: unified diff of `path` against `base` (a commit), or against HEAD when absent.
    /// Untracked files are diffed against an empty file.
    GitDiff { cwd: String, path: String, base: Option<String> },
    /// Discards uncommitted changes to `path` (restores it from HEAD; deletes it if untracked).
    GitRevert { cwd: String, path: String },
}

impl Request {
    /// Requests that may take a while and can safely run out of order.
    pub fn is_slow(&self) -> bool {
        matches!(self, Request::AuthStatus | Request::GitStatus { .. } | Request::GitDiff { .. })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GitStatus {
    /// Repository root (absolute).
    pub root: String,
    /// Current branch, or None when HEAD is detached.
    pub branch: Option<String>,
    /// What the branch is compared against, e.g. `origin/main`, and the merge-base commit.
    pub base: Option<String>,
    pub merge_base: Option<String>,
    /// Working tree vs HEAD, including untracked files.
    pub uncommitted: Vec<GitFile>,
    /// Merge-base vs working tree (tracked files).
    pub branch_files: Vec<GitFile>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GitFile {
    /// Absolute path.
    pub path: String,
    /// Path relative to the repository root.
    pub rel: String,
    /// One letter: M modified, A added, D deleted, R renamed, ? untracked, U conflict.
    pub status: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ImageAttachment {
    /// e.g. `image/png`
    pub media_type: String,
    /// base64-encoded bytes
    pub data: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ServerMsg {
    Response {
        id: u64,
        ok: bool,
        #[serde(default)]
        data: Value,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        error: Option<String>,
    },
    Event { event: Event },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Event {
    SessionsChanged { sessions: Vec<SessionInfo> },
    Session {
        session: String,
        #[serde(flatten)]
        event: SeqEvent,
    },
    LoginOutput { text: String },
    LoginFinished { success: bool },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionStatus {
    #[default]
    Idle,
    Busy,
    WaitingPermission,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionInfo {
    pub id: String,
    pub name: String,
    pub cwd: String,
    /// Agent backend; only `"claude"` for now.
    pub backend: String,
    #[serde(default)]
    pub permission_mode: Option<String>,
    #[serde(default)]
    pub status: SessionStatus,
    /// Unix seconds.
    pub created_at: u64,
}

/// A session event with its per-session sequence number. Clients use `seq` to
/// stitch a `GetHistory` snapshot together with live events without duplicates.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SeqEvent {
    pub seq: u64,
    pub event: SessionEvent,
}

/// Agent-agnostic transcript events.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum SessionEvent {
    User { text: String, images: usize },
    /// Streaming text; not stored in history (the final `Text` replaces it).
    TextDelta { text: String },
    Text { text: String },
    ToolUse { id: String, name: String, input: Value },
    ToolResult { tool_use_id: String, content: String, is_error: bool },
    PermissionRequest {
        request_id: String,
        tool_name: String,
        input: Value,
        #[serde(default)]
        description: Option<String>,
    },
    PermissionResolved { request_id: String, allowed: bool },
    TurnComplete {
        is_error: bool,
        #[serde(default)]
        duration_ms: Option<u64>,
        #[serde(default)]
        cost_usd: Option<f64>,
        #[serde(default)]
        result: Option<String>,
    },
    Info { message: String },
    Error { message: String },
}

impl SessionEvent {
    pub fn is_transient(&self) -> bool {
        matches!(self, SessionEvent::TextDelta { .. })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DirEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
    pub size: u64,
}

/// Unix socket the daemon listens on. Override with `CODEVIEW_SOCKET`.
pub fn socket_path() -> PathBuf {
    if let Some(p) = std::env::var_os("CODEVIEW_SOCKET") {
        return p.into();
    }
    let base = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let user = std::env::var("USER").unwrap_or_else(|_| "user".into());
            std::env::temp_dir().join(format!("codeview-{user}"))
        });
    base.join("codeview").join("daemon.sock")
}

/// Persistent daemon state. Override with `CODEVIEW_STATE_DIR`.
pub fn state_dir() -> PathBuf {
    if let Some(p) = std::env::var_os("CODEVIEW_STATE_DIR") {
        return p.into();
    }
    let base = std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| home_dir().join(".local/state"));
    base.join("codeview")
}

pub fn home_dir() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| "/".into())
}
