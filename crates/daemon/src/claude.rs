//! Drives a headless Claude Code process:
//! `claude -p --input-format stream-json --output-format stream-json ...`
//! Permission prompts arrive as `control_request`/`can_use_tool` on stdout and
//! are answered with `control_response` on stdin (`--permission-prompt-tool stdio`).

use crate::daemon::Daemon;
use anyhow::{Context, Result};
use codeview_protocol::{ImageAttachment, SessionEvent};
use serde_json::{Value, json};
use std::process::Stdio;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;
use tokio::sync::{mpsc, oneshot};

const MAX_TOOL_RESULT_CHARS: usize = 4000;

/// Handle to a running Claude process. Dropping it kills the process.
pub struct ClaudeProc {
    pub generation: u64,
    stdin: mpsc::UnboundedSender<String>,
    _kill: oneshot::Sender<()>,
}

impl ClaudeProc {
    pub fn send(&self, line: String) {
        let _ = self.stdin.send(line);
    }
}

pub struct SpawnArgs {
    pub bin: String,
    pub session: String,
    pub generation: u64,
    pub cwd: String,
    pub claude_session_id: String,
    pub resume: bool,
    pub permission_mode: Option<String>,
}

/// What the daemon should do in response to one line of Claude output.
pub enum Action {
    Event(SessionEvent),
    Init { session_id: String, model: Option<String> },
    Permission { request_id: String, tool_name: String, input: Value, description: Option<String> },
    /// A line to write back to Claude's stdin.
    Reply(String),
}

/// Environment variables that make a child `claude` think it's nested inside another session.
pub fn scrub_env(cmd: &mut Command) {
    cmd.env_remove("CLAUDECODE").env_remove("CLAUDE_CODE_ENTRYPOINT");
}

pub fn spawn(daemon: Arc<Daemon>, a: SpawnArgs) -> Result<ClaudeProc> {
    let mut cmd = Command::new(&a.bin);
    cmd.args([
        "-p",
        "--input-format", "stream-json",
        "--output-format", "stream-json",
        "--verbose",
        "--include-partial-messages",
        "--permission-prompt-tool", "stdio",
        // The GUI can't answer AskUserQuestion yet; Claude asks in plain text instead.
        "--disallowed-tools", "AskUserQuestion",
    ]);
    if a.resume {
        cmd.args(["--resume", &a.claude_session_id]);
    } else {
        cmd.args(["--session-id", &a.claude_session_id]);
    }
    if let Some(mode) = &a.permission_mode {
        cmd.args(["--permission-mode", mode]);
    }
    scrub_env(&mut cmd);
    cmd.current_dir(&a.cwd)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    let mut child = cmd.spawn().with_context(|| format!("starting {}", a.bin))?;

    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    let (kill_tx, kill_rx) = oneshot::channel::<()>();

    tokio::spawn(async move {
        while let Some(mut line) = rx.recv().await {
            line.push('\n');
            if stdin.write_all(line.as_bytes()).await.is_err() || stdin.flush().await.is_err() {
                break;
            }
        }
    });

    let (session, generation) = (a.session.clone(), a.generation);
    let d = daemon.clone();
    let reader = tokio::spawn(async move {
        let mut lines = BufReader::new(stdout).lines();
        while let Ok(Some(line)) = lines.next_line().await {
            match serde_json::from_str::<Value>(&line) {
                Ok(v) => {
                    let actions = translate(&v);
                    if !actions.is_empty() {
                        d.on_claude_actions(&session, generation, actions);
                    }
                }
                Err(_) => eprintln!("claude: non-JSON output: {line}"),
            }
        }
    });

    let stderr_task = tokio::spawn(async move {
        let mut buf = Vec::new();
        let _ = stderr.read_to_end(&mut buf).await;
        let text = String::from_utf8_lossy(&buf).into_owned();
        let start = text.len().saturating_sub(2000);
        text[text.ceil_char_boundary(start)..].to_string()
    });

    let session = a.session.clone();
    tokio::spawn(async move {
        let code = tokio::select! {
            status = child.wait() => status.ok().and_then(|s| s.code()),
            _ = kill_rx => {
                let _ = child.kill().await;
                None
            }
        };
        let _ = reader.await;
        let tail = stderr_task.await.unwrap_or_default();
        daemon.on_claude_exit(&session, generation, code, tail);
    });

    let proc = ClaudeProc { generation: a.generation, stdin: tx, _kill: kill_tx };
    proc.send(json!({"type": "control_request", "request_id": "init", "request": {"subtype": "initialize"}}).to_string());
    Ok(proc)
}

pub fn user_message(text: &str, images: &[ImageAttachment]) -> String {
    let mut content = Vec::new();
    if !text.is_empty() {
        content.push(json!({"type": "text", "text": text}));
    }
    for img in images {
        content.push(json!({
            "type": "image",
            "source": {"type": "base64", "media_type": img.media_type, "data": img.data},
        }));
    }
    json!({"type": "user", "message": {"role": "user", "content": content}}).to_string()
}

pub fn interrupt_request() -> String {
    json!({
        "type": "control_request",
        "request_id": uuid::Uuid::new_v4().to_string(),
        "request": {"subtype": "interrupt"},
    })
    .to_string()
}

pub fn permission_response(request_id: &str, allow: bool, input: Value) -> String {
    let response = if allow {
        json!({"behavior": "allow", "updatedInput": input})
    } else {
        json!({"behavior": "deny", "message": "The user denied this action."})
    };
    json!({
        "type": "control_response",
        "response": {"subtype": "success", "request_id": request_id, "response": response},
    })
    .to_string()
}

fn translate(v: &Value) -> Vec<Action> {
    let mut out = Vec::new();
    // Messages from subagents carry a parent tool id; keep the main transcript readable.
    if v.get("parent_tool_use_id").is_some_and(|p| !p.is_null()) {
        return out;
    }
    match v["type"].as_str().unwrap_or_default() {
        "stream_event" => {
            let e = &v["event"];
            if e["type"] == "content_block_delta" && e["delta"]["type"] == "text_delta" {
                if let Some(t) = e["delta"]["text"].as_str() {
                    out.push(Action::Event(SessionEvent::TextDelta { text: t.to_string() }));
                }
            }
        }
        "system" if v["subtype"] == "init" => {
            out.push(Action::Init {
                session_id: v["session_id"].as_str().unwrap_or_default().to_string(),
                model: v["model"].as_str().map(str::to_string),
            });
        }
        "assistant" => {
            for block in v["message"]["content"].as_array().into_iter().flatten() {
                match block["type"].as_str() {
                    Some("text") => {
                        let text = block["text"].as_str().unwrap_or_default();
                        if !text.is_empty() {
                            out.push(Action::Event(SessionEvent::Text { text: text.to_string() }));
                        }
                    }
                    Some("tool_use") => out.push(Action::Event(SessionEvent::ToolUse {
                        id: block["id"].as_str().unwrap_or_default().to_string(),
                        name: block["name"].as_str().unwrap_or_default().to_string(),
                        input: block["input"].clone(),
                    })),
                    _ => {}
                }
            }
        }
        "user" => {
            for block in v["message"]["content"].as_array().into_iter().flatten() {
                if block["type"] == "tool_result" {
                    out.push(Action::Event(SessionEvent::ToolResult {
                        tool_use_id: block["tool_use_id"].as_str().unwrap_or_default().to_string(),
                        content: truncate(tool_result_text(&block["content"])),
                        is_error: block["is_error"].as_bool().unwrap_or(false),
                    }));
                }
            }
        }
        "result" => {
            let is_error = v["is_error"].as_bool().unwrap_or(false);
            out.push(Action::Event(SessionEvent::TurnComplete {
                is_error,
                duration_ms: v["duration_ms"].as_u64(),
                cost_usd: v["total_cost_usd"].as_f64(),
                result: if is_error {
                    v["result"].as_str().or(v["subtype"].as_str()).map(str::to_string)
                } else {
                    None
                },
            }));
        }
        "control_request" => {
            let request_id = v["request_id"].as_str().unwrap_or_default().to_string();
            let req = &v["request"];
            if req["subtype"] == "can_use_tool" {
                out.push(Action::Permission {
                    request_id,
                    tool_name: req["tool_name"].as_str().unwrap_or_default().to_string(),
                    input: req["input"].clone(),
                    description: req["description"].as_str().map(str::to_string),
                });
            } else {
                // Unknown host callbacks must still be answered or Claude will wait forever.
                out.push(Action::Reply(
                    json!({
                        "type": "control_response",
                        "response": {"subtype": "error", "request_id": request_id, "error": "not supported by codeview"},
                    })
                    .to_string(),
                ));
            }
        }
        _ => {}
    }
    out
}

fn tool_result_text(content: &Value) -> String {
    match content {
        Value::String(s) => s.clone(),
        Value::Array(blocks) => blocks
            .iter()
            .map(|b| match b["type"].as_str() {
                Some("text") => b["text"].as_str().unwrap_or_default().to_string(),
                Some("image") => "[image]".to_string(),
                _ => String::new(),
            })
            .collect::<Vec<_>>()
            .join("\n"),
        _ => String::new(),
    }
}

fn truncate(mut s: String) -> String {
    if s.len() > MAX_TOOL_RESULT_CHARS {
        let cut = s.floor_char_boundary(MAX_TOOL_RESULT_CHARS);
        s.truncate(cut);
        s.push_str("\n…");
    }
    s
}
