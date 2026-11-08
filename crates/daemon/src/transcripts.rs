//! Reads Claude Code's own conversation transcripts (`~/.claude/projects/<folder>/<id>.jsonl`),
//! so conversations started anywhere — including a terminal — can be resumed here.

use crate::claude::{self, Action};
use anyhow::{Result, bail};
use codeview_protocol::{Conversation, SessionEvent};
use serde_json::Value;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Most history events loaded for a resumed conversation (the newest are kept).
const MAX_HISTORY: usize = 2000;
/// A transcript written to this recently may still be in use by another Claude.
const RECENT: Duration = Duration::from_secs(120);

fn project_dir(cwd: &str) -> PathBuf {
    let base = std::env::var_os("CLAUDE_CONFIG_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| codeview_protocol::home_dir().join(".claude"));
    // Claude names the folder after the working directory with every other character replaced by '-'.
    let name: String = cwd.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '-' }).collect();
    base.join("projects").join(name)
}

pub fn transcript_path(cwd: &str, id: &str) -> Result<PathBuf> {
    if id.is_empty() || !id.chars().all(|c| c.is_ascii_hexdigit() || c == '-') {
        bail!("invalid conversation id");
    }
    let path = project_dir(cwd).join(format!("{id}.jsonl"));
    if !path.is_file() {
        bail!("no saved conversation {id} for {cwd}");
    }
    Ok(path)
}

/// The text and image count of a prompt the user typed, or None for tool results,
/// internal messages and lines from subagents.
fn prompt(v: &Value) -> Option<(String, usize)> {
    if v["type"] != "user" || v["isMeta"] == true || v["isSidechain"] == true {
        return None;
    }
    let (text, images) = match &v["message"]["content"] {
        Value::String(s) => (s.clone(), 0),
        Value::Array(blocks) => {
            if blocks.iter().any(|b| b["type"] == "tool_result") {
                return None;
            }
            let text = blocks.iter().filter_map(|b| b["text"].as_str()).collect::<Vec<_>>().join("\n");
            (text, blocks.iter().filter(|b| b["type"] == "image").count())
        }
        _ => return None,
    };
    // Slash-command wrappers and interruption markers aren't prompts.
    let t = text.trim();
    if t.starts_with('<') || t.starts_with("[Request interrupted") || (t.is_empty() && images == 0) {
        return None;
    }
    Some((text, images))
}

fn lines(path: &PathBuf) -> impl Iterator<Item = Value> {
    let file = std::fs::File::open(path).ok();
    file.into_iter()
        .flat_map(|f| BufReader::new(f).lines())
        .map_while(|l| l.ok())
        .filter_map(|l| serde_json::from_str(&l).ok())
}

/// Saved conversations for `cwd`, newest first. `open` maps Claude session ids to codeview sessions.
pub fn list(cwd: &str, open: &dyn Fn(&str) -> Option<String>) -> Vec<Conversation> {
    let Ok(dir) = std::fs::read_dir(project_dir(cwd)) else { return Vec::new() };
    let now = SystemTime::now();
    let mut out = Vec::new();
    for entry in dir.flatten() {
        let path = entry.path();
        if path.extension().is_none_or(|e| e != "jsonl") {
            continue;
        }
        let Some(id) = path.file_stem().map(|s| s.to_string_lossy().into_owned()) else { continue };
        let modified = entry.metadata().and_then(|m| m.modified()).unwrap_or(UNIX_EPOCH);
        let (mut title, mut custom, mut prompts) = (String::new(), None, 0);
        for v in lines(&path) {
            if let Some(t) = v["customTitle"].as_str() {
                custom = Some(t.to_string());
            } else if let Some((text, _)) = prompt(&v) {
                if prompts == 0 {
                    title = text.split_whitespace().collect::<Vec<_>>().join(" ");
                }
                prompts += 1;
            }
        }
        if prompts == 0 {
            continue;
        }
        let mut title = custom.unwrap_or(title);
        if title.chars().count() > 120 {
            title = title.chars().take(120).collect::<String>() + "…";
        }
        out.push(Conversation {
            open_session: open(&id),
            id,
            title,
            prompts,
            modified: modified.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
            recent: now.duration_since(modified).is_ok_and(|d| d < RECENT),
        });
    }
    out.sort_by(|a, b| b.modified.cmp(&a.modified));
    out
}

/// A transcript as chat history events, plus the title of the conversation.
pub fn history(path: &PathBuf) -> (Vec<SessionEvent>, String) {
    let mut events = Vec::new();
    let mut title = String::new();
    for v in lines(path) {
        if let Some((text, images)) = prompt(&v) {
            if title.is_empty() {
                title = text.split_whitespace().collect::<Vec<_>>().join(" ");
            }
            events.push(SessionEvent::User { text, images });
        } else if v["isSidechain"] != true && (v["type"] == "assistant" || v["type"] == "user") {
            events.extend(claude::translate(&v).into_iter().filter_map(|a| match a {
                Action::Event(e) => Some(e),
                _ => None,
            }));
        }
    }
    if events.len() > MAX_HISTORY {
        events.drain(..events.len() - MAX_HISTORY);
        events.insert(0, SessionEvent::Info { message: "Older messages aren't shown.".into() });
    }
    (events, title)
}
